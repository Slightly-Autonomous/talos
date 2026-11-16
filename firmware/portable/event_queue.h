// Bounded outbound queue with priorities, in-flight tracking and retry
// backoff (spec §35 "bounded queues"). Fixed capacity, no heap.
//
// Loss policy when full: drop the oldest event of the LOWEST priority present;
// a P0 is only dropped when the whole queue is P0 (counted in dropped_p0).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "firmware/portable/types.h"

namespace talos::fw {

template <std::size_t N>
class EventQueue {
 public:
  struct Slot {
    DeviceEvent event;
    bool used = false;
    bool in_flight = false;
    TimeMs next_send_ms = 0;
    std::uint32_t backoff_ms = 0;
    std::uint64_t order = 0;  // FIFO tie-break
  };

  static constexpr std::uint32_t kInitialBackoffMs = 500;
  static constexpr std::uint32_t kMaxBackoffMs = 8000;

  void push(const DeviceEvent& e) {
    Slot* free = nullptr;
    for (auto& s : slots_) {
      if (!s.used) {
        free = &s;
        break;
      }
    }
    if (!free) free = evict();
    *free = Slot{e, true, false, 0, kInitialBackoffMs, next_order_++};
  }

  // Next event due for (re)transmission: highest priority, then oldest.
  Slot* next_due(TimeMs now) {
    Slot* best = nullptr;
    for (auto& s : slots_) {
      if (!s.used || s.next_send_ms > now) continue;
      if (!best || s.event.priority < best->event.priority ||
          (s.event.priority == best->event.priority && s.order < best->order)) {
        best = &s;
      }
    }
    return best;
  }

  // Bounded exponential backoff after each send attempt.
  void mark_sent(Slot& s, TimeMs now) {
    s.in_flight = true;
    s.next_send_ms = now + s.backoff_ms;
    s.backoff_ms = s.backoff_ms * 2 > kMaxBackoffMs ? kMaxBackoffMs : s.backoff_ms * 2;
  }

  // An ACK names (boot_epoch, seq): events restored from a previous boot keep
  // their original identity, so the hub deduplicates them.
  bool ack(std::uint32_t boot_epoch, std::uint32_t seq) {
    for (auto& s : slots_) {
      if (s.used && s.event.boot_epoch == boot_epoch && s.event.seq == seq) {
        s.used = false;
        return true;
      }
    }
    return false;
  }

  std::size_t size() const {
    std::size_t n = 0;
    for (const auto& s : slots_) n += s.used ? 1 : 0;
    return n;
  }

  std::optional<TimeMs> earliest_due() const {
    std::optional<TimeMs> t;
    for (const auto& s : slots_) {
      if (s.used && (!t || s.next_send_ms < *t)) t = s.next_send_ms;
    }
    return t;
  }

  template <typename F>
  void for_each(F&& f) const {
    for (const auto& s : slots_) {
      if (s.used) f(s.event);
    }
  }

  std::uint32_t dropped() const { return dropped_; }
  std::uint32_t dropped_p0() const { return dropped_p0_; }

 private:
  Slot* evict() {
    Slot* victim = &slots_[0];
    for (auto& s : slots_) {
      if (s.event.priority > victim->event.priority ||
          (s.event.priority == victim->event.priority && s.order < victim->order)) {
        victim = &s;
      }
    }
    ++dropped_;
    if (victim->event.priority == Priority::kP0Critical) ++dropped_p0_;
    return victim;
  }

  std::array<Slot, N> slots_{};
  std::uint64_t next_order_ = 0;
  std::uint32_t dropped_ = 0;
  std::uint32_t dropped_p0_ = 0;
};

}  // namespace talos::fw
