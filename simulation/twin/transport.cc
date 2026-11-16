#include "simulation/twin/transport.h"

#include <algorithm>

namespace talos::sim {

Transport::Transport(
    const google::protobuf::RepeatedPtrField<talos::sim::v1::LinkProfile>& profiles,
    std::uint64_t seed)
    : rng_(Rng::stream(seed, "transport")) {
  default_.set_latency_ms(50);
  for (const auto& p : profiles) {
    if (p.device_id().empty()) {
      default_ = p;
    } else {
      profiles_[p.device_id()] = p;
    }
  }
}

const talos::sim::v1::LinkProfile& Transport::profile(const std::string& device) const {
  auto it = profiles_.find(device);
  return it == profiles_.end() ? default_ : it->second;
}

void Transport::set_connected(const std::string& device, bool connected) {
  if (connected) {
    disconnected_.erase(device);
  } else {
    disconnected_.insert(device);
  }
}

void Transport::send(std::int64_t now_ms, const std::string& device, bool uplink, Body body) {
  if (!connected(device)) {
    ++dropped_;
    return;
  }
  const auto& p = profile(device);
  const int copies = rng_.chance(p.duplicate_probability()) ? 2 : 1;
  for (int i = 0; i < copies; ++i) {
    if (rng_.chance(p.loss_probability())) {
      ++dropped_;
      continue;
    }
    const auto jitter = rng_.between(0, p.jitter_ms());
    in_flight_.push_back({now_ms + p.latency_ms() + jitter, next_order_++, device, uplink, body});
  }
}

std::vector<Message> Transport::due(std::int64_t now_ms) {
  std::vector<Message> ready, later;
  for (auto& m : in_flight_) {
    (m.deliver_at_ms <= now_ms ? ready : later).push_back(std::move(m));
  }
  in_flight_.swap(later);
  std::sort(ready.begin(), ready.end(), [](const Message& a, const Message& b) {
    return a.deliver_at_ms != b.deliver_at_ms ? a.deliver_at_ms < b.deliver_at_ms
                                              : a.order < b.order;
  });
  // A link that went down while the message was in flight loses it.
  std::vector<Message> delivered;
  for (auto& m : ready) {
    if (connected(m.device)) {
      delivered.push_back(std::move(m));
    } else {
      ++dropped_;
    }
  }
  return delivered;
}

}  // namespace talos::sim
