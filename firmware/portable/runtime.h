// NodeRuntime: the portable device firmware core (spec §35).
//
//   boot -> restore -> discover modules -> init drivers -> admit power
//   loop: wake(interrupt|timer) -> poll drivers -> queue events
//         -> transmit with bounded retry -> handle downlink -> sleep
//
// The platform calls boot() once, on_interrupt() from wake sources, and
// step() whenever awake; next_wake_ms() says when to wake next.
#pragma once

#include <array>
#include <cstdint>

#include "firmware/portable/event_queue.h"
#include "firmware/portable/hal.h"
#include "firmware/portable/modules.h"
#include "firmware/portable/ota.h"

namespace talos::fw {

class NodeRuntime final : public IEventSink {
 public:
  static constexpr std::size_t kQueueCapacity = 32;
  static constexpr std::size_t kMaxPendingCritical = 8;
  static constexpr std::uint32_t kLowPowerMv = 3550;
  static constexpr std::uint32_t kCriticalMv = 3300;
  static constexpr std::uint32_t kOtaMinMv = 3700;  // or external power
  static constexpr int kSendsPerStep = 4;

  explicit NodeRuntime(Platform& p) : p_(p), ota_(p) {}

  void boot();
  void on_interrupt() { awake_ = true; }
  void step();
  // Earliest time the runtime needs to run again (now while debouncing).
  TimeMs next_wake_ms() const;
  bool reboot_requested() const { return reboot_requested_; }

  // IEventSink
  void emit(std::uint8_t slot, EventKind kind, Priority priority, bool flag, std::int32_t a,
            std::uint32_t b) override;

  // Introspection (diagnostics and tests).
  std::uint32_t boot_epoch() const { return boot_epoch_; }
  const SlotState& slot(std::uint8_t i) const { return slots_[i]; }
  std::size_t queued() const { return queue_.size(); }
  const RuntimeConfig& config() const { return config_; }
  const OtaRecord& ota() const { return ota_.record(); }
  std::uint32_t running_version() const { return ota_.running_version(); }
  PowerMode power_mode() const { return mode_; }
  std::uint32_t dropped_events() const { return queue_.dropped(); }

 private:
  void discover();
  void admit_power();
  PowerMode compute_mode();
  void handle(const Downlink& d);
  void persist_pending_critical();
  void restore_pending_critical();
  std::uint32_t sustained_budget_mw();

  Platform& p_;
  Ota ota_;
  EventQueue<kQueueCapacity> queue_;
  std::array<SlotState, kSlots> slots_{};
  RuntimeConfig config_;
  std::uint32_t boot_epoch_ = 0;
  std::uint32_t seq_ = 0;
  PowerMode mode_ = PowerMode::kNormal;
  TimeMs next_heartbeat_ = 0;
  bool awake_ = true;
  bool reboot_requested_ = false;
};

}  // namespace talos::fw
