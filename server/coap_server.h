// CoAP/UDP device ingress (ADR 0014). Thread border routers forward node
// traffic here. Per request: CoAP parse -> per-device key -> HMAC-verified
// frame -> per-device token bucket (authenticated frames only) ->
// HubService::device_exchange -> authenticated piggybacked reply. Frames that
// fail authentication spend a separate per-peer bucket, so forged traffic
// for a device id can never throttle that device's genuine frames.
// Only FRESH verified frames (carrying an event the hub has not seen, and a
// tag not seen recently) spend the device's bucket; a replayed or
// retransmitted frame spends a per-peer replay bucket instead, so a LAN
// attacker replaying a sniffed frame mutes only itself (red team #3).
// A retransmitted CON gets the cached reply instead of being processed
// twice; only replies to authenticated requests are cached, keyed by peer
// and message id and matched on the identical request bytes, so a spoofed
// datagram can never poison what the real node receives (red team #10).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/base/result.h"
#include "server/hub_service.h"

namespace talos::server {

using DeviceKeyMap = std::map<std::string, std::vector<std::uint8_t>>;

// Loads a DeviceKeys YAML file; every device must exist in the property and
// keys must be at least 16 bytes.
Result<DeviceKeyMap> load_device_keys(const std::string& path, const Property& property);

class CoapServer {
 public:
  CoapServer(HubService& hub, DeviceKeyMap keys, std::uint32_t rate_per_second,
             std::uint32_t burst);
  ~CoapServer();
  CoapServer(const CoapServer&) = delete;
  CoapServer& operator=(const CoapServer&) = delete;

  // Binds UDP; port 0 picks a free port. Returns the bound port.
  Result<int> bind(const std::string& host, int port);
  void start();
  void stop();

  // Exposed for tests: handles one datagram and returns the reply bytes.
  std::vector<std::uint8_t> handle(const std::vector<std::uint8_t>& datagram,
                                   const std::string& peer);

 private:
  void loop();
  struct Bucket {
    double tokens = 0;
    std::chrono::steady_clock::time_point last;
  };
  bool allow(std::map<std::string, Bucket>& buckets, const std::string& key);
  struct CachedReply {
    std::string peer;
    std::uint16_t message_id;
    std::vector<std::uint8_t> request;  // exact bytes of the authenticated request
    std::vector<std::uint8_t> reply;
  };
  // Recently seen uplink tags per device (bounded), for replay detection.
  bool tag_seen(const std::string& device, std::uint64_t tag) const;
  void remember_tag(const std::string& device, std::uint64_t tag);
  struct RecentTags {
    std::deque<std::uint64_t> order;
    std::set<std::uint64_t> tags;
  };
  static constexpr std::size_t kRecentTags = 256;

  HubService& hub_;
  DeviceKeyMap keys_;
  double rate_;
  double burst_;
  std::map<std::string, Bucket> buckets_;          // authenticated, per device
  std::map<std::string, Bucket> failure_buckets_;  // failed auth, per peer host
  std::map<std::string, Bucket> replay_buckets_;   // verified but stale, per peer host
  std::map<std::string, RecentTags> recent_tags_;  // per device
  std::deque<CachedReply> cache_;
  int fd_ = -1;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace talos::server
