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
#include <optional>

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
  static constexpr std::uint8_t kNotPowering = 0xFF;
  // Time AON is held on, after set_rail() returns, before the attempt marker is
  // cleared: a short pulls the core rail down well inside it. With the fitted
  // 1 nF CT cap the TPS22917 turns on in ~3.8 ms typ (SLVSDW8B, no max given);
  // BoardIo already waits BoardIo::kAonSettleMs (10 ms) inside set_rail(), so
  // the marker spans ~20 ms, ~5x typ tON. Provisional (bench B2, L-13/L-14).
  static constexpr std::uint32_t kSlotPowerSettleMs = 10;
  // A reset while powering a slot is a strike against it; this many in a row
  // quarantine it (one unrelated reset, e.g. a USB unplug, does not).
  static constexpr std::uint8_t kQuarantineStrikes = 2;
  // A quarantined slot is retried after this much uptime, doubling per further
  // strike up to kQuarantineRetryMaxMs: a card that really shorts AON costs at
  // most one node reset per retry, and a false positive heals by itself.
  static constexpr std::uint32_t kQuarantineRetryMs = 60u * 60u * 1000u;           // 1 h
  static constexpr std::uint32_t kQuarantineRetryMaxMs = 24u * 60u * 60u * 1000u;  // 24 h
  // While the boot epoch is not persisted, nothing is transmitted (an event
  // must never reuse an (epoch, seq) the hub has seen); the write is retried.
  static constexpr std::uint32_t kEpochRetryMs = 1000;

  explicit NodeRuntime(Platform& p, IWatchdog* watchdog = nullptr)
      : p_(p), ota_(p), watchdog_(watchdog) {}

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
  // Re-run discovery and driver init for one slot, or all (bench: after a rail
  // change or a self-test that reconfigured a sensor).
  void rescan(std::optional<std::uint8_t> slot = std::nullopt);
  // False if the board's expanders or charger did not take their safe state at boot.
  bool board_ok() const { return board_ok_; }
  // False while the boot epoch could not be written: events are held, not sent.
  bool boot_epoch_persisted() const { return epoch_persisted_; }
  // Slots held unpowered because the node reset while switching their AON on.
  std::uint8_t quarantined_slots() const { return quarantined_; }
  // Operator action (diag `rail <slot> aon on`): let the slot be powered again.
  void clear_quarantine(std::uint8_t slot);
  // Consecutive resets seen while powering the slot (cleared once it powers up).
  std::uint8_t power_strikes(std::uint8_t slot) const { return strikes_[slot]; }

 private:
  void discover();
  void discover_slot(std::uint8_t i, TimeMs now);
  void admit_power();
  bool power_slot(std::uint8_t i);
  // Returns true if the slot's AON should stay on (the card is usable or needs it for diagnosis).
  bool init_slot(std::uint8_t i, SlotIo& io, TimeMs now);
  void unpower_slot(std::uint8_t i);
  void save_slot_power(std::uint8_t powering);
  void retry_quarantined(TimeMs now);
  bool persist_boot_epoch();
  void feed_watchdog() {
    if (watchdog_) watchdog_->feed();
  }
  PowerMode compute_mode();
  void handle(const Downlink& d);
  void emit_ota_status(Priority priority);
  void persist_pending_critical();
  void restore_pending_critical();
  std::uint32_t sustained_budget_mw();

  Platform& p_;
  Ota ota_;
  IWatchdog* watchdog_ = nullptr;
  EventQueue<kQueueCapacity> queue_;
  std::array<SlotState, kSlots> slots_{};
  RuntimeConfig config_;
  std::uint32_t boot_epoch_ = 0;
  std::uint32_t seq_ = 0;
  PowerMode mode_ = PowerMode::kNormal;
  TimeMs next_heartbeat_ = 0;
  bool awake_ = true;
  bool reboot_requested_ = false;
  bool ota_progress_ = false;
  bool board_ok_ = true;
  std::uint8_t quarantined_ = 0;
  std::uint8_t aon_on_ = 0;  // AON rails this runtime switched on
  std::array<std::uint8_t, kSlots> strikes_{};
  std::array<TimeMs, kSlots> quarantine_retry_at_{};
  bool epoch_persisted_ = false;
  TimeMs next_epoch_retry_ = 0;
};

}  // namespace talos::fw
