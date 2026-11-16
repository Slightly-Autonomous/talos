// Virtual link between devices and the hub (spec §41 "virtual networking"):
// per-device latency, jitter, loss, duplication and disconnects, all seeded.
// Reordering emerges from jitter. Device-side store-and-forward and retries
// belong to the portable firmware (Phase 5) and real Thread behavior to the
// OpenThread/OTNS harness (Phase 6).
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "core/events/observation.h"
#include "core/execution/command.h"
#include "simulation/base/rng.h"
#include "talos/sim/v1/scenario.pb.h"

namespace talos::sim {

using Body = std::variant<ObservationEnvelope, CommandAck, Command>;

struct Message {
  std::int64_t deliver_at_ms = 0;
  std::uint64_t order = 0;  // tie-break: send order
  std::string device;
  bool uplink = true;
  Body body;
};

class Transport {
 public:
  Transport(const google::protobuf::RepeatedPtrField<talos::sim::v1::LinkProfile>& profiles,
            std::uint64_t seed);

  void set_connected(const std::string& device, bool connected);
  bool connected(const std::string& device) const { return !disconnected_.contains(device); }

  void send(std::int64_t now_ms, const std::string& device, bool uplink, Body body);
  // Messages due at or before now, in (deliver_at, send order) order.
  std::vector<Message> due(std::int64_t now_ms);

  std::uint64_t dropped() const { return dropped_; }

 private:
  const talos::sim::v1::LinkProfile& profile(const std::string& device) const;

  std::map<std::string, talos::sim::v1::LinkProfile> profiles_;
  talos::sim::v1::LinkProfile default_;
  Rng rng_;
  std::set<std::string> disconnected_;
  std::vector<Message> in_flight_;
  std::uint64_t next_order_ = 0;
  std::uint64_t dropped_ = 0;
};

}  // namespace talos::sim
