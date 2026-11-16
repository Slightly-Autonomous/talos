// ILink over CoAP (ADR 0014). Each event goes out as a confirmable
// POST /d/<device_id>/e carrying a wire-format frame; the hub's piggybacked
// 2.04 reply carries authenticated downlink items (ACKs, config, OTA). The
// runtime's queue owns retries, so a lost exchange is simply resent with a
// fresh message id and the hub deduplicates by (boot_epoch, seq).
#pragma once

#include <array>
#include <cstdint>

#include "firmware/portable/hal.h"
#include "firmware/portable/wire.h"

namespace talos::fw {

class CoapLink final : public ILink {
 public:
  static constexpr std::size_t kMaxIdLen = 31;
  static constexpr std::size_t kPending = 8;
  static constexpr std::size_t kInbox = 32;

  // `device_id` and `key` must outlive the link.
  CoapLink(IDatagram& net, const char* device_id, wire::Key key);

  bool send(const DeviceEvent& e) override;
  std::optional<Downlink> receive() override;

  std::uint32_t rejected_replies() const { return rejected_; }

 private:
  void poll_network();

  struct PendingRequest {
    bool used = false;
    std::array<std::uint8_t, 4> token{};
    wire::Tag tag{};
  };

  IDatagram& net_;
  const char* device_id_;
  wire::Key key_;
  std::uint16_t next_mid_ = 1;
  std::uint32_t next_token_ = 1;
  std::array<PendingRequest, kPending> pending_{};
  std::size_t pending_next_ = 0;
  std::array<Downlink, kInbox> inbox_{};
  std::size_t inbox_head_ = 0;
  std::size_t inbox_size_ = 0;
  std::uint32_t rejected_ = 0;
};

}  // namespace talos::fw
