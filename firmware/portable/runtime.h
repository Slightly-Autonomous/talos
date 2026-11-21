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
#include "firmware/portable/indicators.h"
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
  // ConfigUpdate bounds (red team RT-14/RT-19): supervision is never stretched
  // past an hour, and environment_interval_ms * 4 can never wrap.
  static constexpr std::uint32_t kMinIntervalMs = 10'000;
  static constexpr std::uint32_t kMaxHeartbeatIntervalMs = 60u * 60u * 1000u;          // 1 h
  static constexpr std::uint32_t kMaxEnvironmentIntervalMs = 24u * 60u * 60u * 1000u;  // 1 day
  // ACKs of persisted alarms are written back at most this often (NVS wear).
  static constexpr std::uint32_t kAckFlushMs = 5u * 60u * 1000u;
  // Link health (red team RT-03): after kLinkFailSends sends and kLinkFailMs
  // with no downlink at all, the link counts as failing. Then one probe goes
  // out per interval, doubling from kProbeMinMs to kProbeMaxMs; a new P0 may
  // add a probe at most every kP0ProbeMinMs. Any downlink ends the outage.
  static constexpr std::uint32_t kLinkFailSends = 8;
  static constexpr std::uint32_t kLinkFailMs = 30'000;
  static constexpr std::uint32_t kProbeMinMs = 10'000;
  static constexpr std::uint32_t kProbeMaxMs = 5u * 60u * 1000u;
  static constexpr std::uint32_t kP0ProbeMinMs = 30'000;
  // Total send budget (token bucket): bursts of kSendBurst, then one send per
  // kSendRefillMs on average, whatever the hub does. P0 bypasses it.
  static constexpr std::uint32_t kSendBurst = 60;
  static constexpr std::uint32_t kSendRefillMs = 2'000;
  // Non-critical telemetry (P2/P3) older than this is dropped, not resent.
  static constexpr std::uint32_t kTelemetryTtlMs = 15u * 60u * 1000u;
  // Downlink items handled per step (the watchdog is fed between them).
  static constexpr int kMaxDownlinksPerStep = 16;
  // A wake line high this long is masked as a deep-sleep wake source (it would
  // re-wake the node forever) and polled every kMaskedPollMs until it falls;
  // after kWakeStuckFaultMs it is reported as stuck (contacts held open excepted).
  static constexpr std::uint32_t kWakeMaskMs = 30'000;
  static constexpr std::uint32_t kMaskedPollMs = 1'000;
  static constexpr std::uint32_t kWakeStuckFaultMs = 10u * 60u * 1000u;

  explicit NodeRuntime(Platform& p, IWatchdog* watchdog = nullptr, ISleep* sleep = nullptr)
      : p_(p), ota_(p, watchdog), watchdog_(watchdog), sleep_(sleep) {}

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
  std::uint32_t expired_events() const { return queue_.expired(); }
  // True while the hub has not answered for a while (sends are rationed).
  bool link_failing() const { return outage_; }
  // Slots whose wake line is masked as a wake source (stuck high).
  std::uint8_t wake_masked_slots() const { return wake_masked_; }
  // True if this boot resumed from deep sleep with the cards still powered.
  bool resumed_from_sleep() const { return resumed_cards_; }
  // Why this boot happened and which wake lines fired (bench S-M19).
  const WakeInfo& wake_info() const { return wake_; }
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

  // Indicators (indicators.h). Service mode lights the slot LEDs on battery
  // for SlotLedPolicy::kServiceMs (diag `led service`).
  void start_service_mode();
  void stop_service_mode();
  bool service_mode() const { return slot_leds_.service_active(p_.clock.now_ms()); }
  StatusCode status_code() const { return status_; }
  bool status_led_lit() const { return status_led_level_; }
  bool slot_led_lit(std::uint8_t slot) const { return (slot_led_shadow_ >> slot) & 1u; }
  // USB or PV present (CHG_PG_N low), as last read.
  bool external_power() const { return external_power_; }
  // Debounced enclosure tamper loop: true = open or cut, nullopt = not known yet.
  std::optional<bool> tamper_open() const { return tamper_.state(); }
  // EXP_WAKE held high after servicing every expander: masked as a wake source.
  bool expander_wake_masked() const { return exp_wake_masked_; }

 private:
  struct Resume {
    bool active = false;
    std::array<std::uint16_t, kSlots> code{};
    std::array<std::uint8_t, kSlots> state{};
  };
  void discover(const Resume& r);
  void discover_slot(std::uint8_t i, TimeMs now, const Resume* r = nullptr);
  void admit_power();
  bool power_slot(std::uint8_t i);
  // Returns true if the slot's AON should stay on (the card is usable or needs it for diagnosis).
  bool init_slot(std::uint8_t i, SlotIo& io, TimeMs now, const Resume* r);
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
  void persist_critical(const DeviceEvent& e);
  void note_ack(std::uint32_t boot_epoch, std::uint32_t seq, TimeMs now);
  void flush_acks();
  // retained_free / retained_gen: the ring's ACK state from LP RAM after a
  // deep-sleep wake (newer than NVS, which is written back lazily).
  void restore_pending_critical(std::uint8_t retained_free,
                                const std::array<std::uint32_t, kMaxPendingCritical>* retained_gen);
  void save_retained();
  void supervise_slots(TimeMs now);
  void supervise_wake_lines(TimeMs now);
  // EXP_WAKE: service the expanders' INT, sample the tamper loop, re-read the source.
  void service_expanders(TimeMs now);
  void sample_tamper(TimeMs now);
  void refresh_external_power();
  void update_indicators(TimeMs now);
  void poll_service_button(TimeMs now);
  std::array<SlotLedMode, kSlots> slot_led_modes() const;
  StatusInputs status_inputs(TimeMs now) const;
  void arm_wake_lines();
  void send_due(TimeMs now);
  TimeMs send_gate_ms(TimeMs now) const;
  bool slot_critical(std::uint8_t slot) const;
  std::uint32_t sustained_budget_mw();

  Platform& p_;
  Ota ota_;
  IWatchdog* watchdog_ = nullptr;
  ISleep* sleep_ = nullptr;
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

  // Deep-sleep wake (ESP32-H2: a reset with the cards still powered).
  WakeInfo wake_{};
  bool resumed_cards_ = false;

  // Unacknowledged P0 events, one NVS record per ring slot (gen 0 = empty).
  std::array<std::uint32_t, kMaxPendingCritical> ring_gen_{};
  std::array<std::uint32_t, kMaxPendingCritical> ring_epoch_{};
  std::array<std::uint32_t, kMaxPendingCritical> ring_seq_{};
  std::uint8_t ring_free_ = 0xFF;  // bit i: slot i empty or acknowledged
  std::uint32_t next_gen_ = 1;
  bool acks_dirty_ = false;  // ring_free_ has bits not yet written to NVS
  TimeMs acks_dirty_since_ = 0;

  // Link health and send budget.
  std::uint32_t unanswered_ = 0;
  TimeMs last_reply_ms_ = 0;
  bool outage_ = false;
  std::uint32_t probe_backoff_ms_ = kProbeMinMs;
  TimeMs next_probe_ms_ = 0;
  TimeMs last_probe_ms_ = 0;
  bool p0_waiting_ = false;  // a P0 was queued since the last outage probe
  std::uint32_t tokens_ = kSendBurst;
  TimeMs last_refill_ms_ = 0;
  bool downlink_more_ = false;

  // Indicators and tamper.
  TamperDebouncer tamper_;
  ServiceButton button_;
  SlotLedPolicy slot_leds_;
  bool external_power_ = false;
  bool hub_heard_ = false;
  TimeMs boot_ms_ = 0;
  StatusCode status_ = StatusCode::kOff;
  TimeMs status_since_ = 0;
  bool status_led_level_ = false;
  bool status_led_known_ = false;  // the GPIO matches status_led_level_
  std::uint8_t slot_led_shadow_ = 0;
  bool slot_leds_known_ = false;    // the expander matches slot_led_shadow_
  TimeMs led_due_ms_ = ~TimeMs{0};  // next status or slot LED edge
  TimeMs exp_wake_high_since_ = 0;
  bool exp_wake_high_ = false;
  bool exp_wake_masked_ = false;
  TimeMs next_exp_poll_ = 0;

  // Wake-line supervision.
  std::array<TimeMs, kSlots> wake_high_since_{};
  std::uint8_t wake_masked_ = 0;
  std::uint8_t wake_stuck_reported_ = 0;
  std::array<TimeMs, kSlots> next_masked_poll_{};
};

}  // namespace talos::fw
