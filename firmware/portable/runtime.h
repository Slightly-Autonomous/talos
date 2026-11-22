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

#include "firmware/portable/console_policy.h"
#include "firmware/portable/event_queue.h"
#include "firmware/portable/hal.h"
#include "firmware/portable/indicators.h"
#include "firmware/portable/modules.h"
#include "firmware/portable/ota.h"
#include "firmware/portable/sleep_policy.h"
#include "firmware/portable/tamper_detect.h"

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
  // A built-in sensor (ADR 0019) that failed or was quarantined is retried after
  // this long, doubling per further failure up to kFixedRetryMaxMs. The PIR's
  // retry is a power cycle of PIR_PWR; the others have no rail to cycle.
  static constexpr std::uint32_t kFixedRetryMs = 60u * 60u * 1000u;           // 1 h
  static constexpr std::uint32_t kFixedRetryMaxMs = 24u * 60u * 60u * 1000u;  // 24 h
  // USER (TL-SN-002): one test report (event + heartbeat + lamp test) and one
  // commissioning request per this long at most. Further presses inside it do
  // nothing, so a jammed, bouncing or abused button can't flood the radio, drain
  // the cells or push older P1 state out of the queue (red team).
  static constexpr std::uint32_t kUserCooldownMs = 10'000;
  // After the enclosure closes (installation or service), the installed pose is
  // captured this long later, once hands are off the node (MountPose baseline).
  static constexpr std::uint32_t kMountSettleMs = 10'000;
  // A pose check runs this long after an impact, once the jolt has died down.
  static constexpr std::uint32_t kMountAfterImpactMs = 2'000;

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
  // for SlotLedPolicy::kServiceMs (diag `led service`). With
  // BoardSupport::lamp_gate = kLidOpen (TL-SN-002) it starts only while the lid
  // reads open; returns whether service mode is on afterwards.
  bool start_service_mode();
  void stop_service_mode();
  // Lamps may light without service mode: external power (LampGate::kExternalPower)
  // or the debounced lid open (LampGate::kLidOpen).
  bool lamps_unattended_ok() const;
  // USB console lock (BoardSupport::console_lock, console_policy.h): true while
  // mutating diag commands are refused (kLidClosed and the lid not known open).
  bool console_locked() const;
  // A refused console command: counted, and reported to the hub as a
  // kConsoleLocked diagnostic at most once per kConsoleReportMs.
  void note_console_refusal();
  std::uint32_t console_refusals() const { return console_refused_total_; }
  bool service_mode() const { return slot_leds_.service_active(p_.clock.now_ms()); }
  StatusCode status_code() const { return status_; }
  bool status_led_lit() const { return status_led_level_; }
  bool slot_led_lit(std::uint8_t slot) const { return (slot_led_shadow_ >> slot) & 1u; }
  // USB or PV present (CHG_PG_N low, or TL-SN-002 USB_PRESENT), as last read.
  bool external_power() const { return external_power_; }
  // Board lamps (TL-SN-002): level as last written, and the USER lamp test.
  bool lamp_lit(Lamp l) const { return (lamp_shadow_ >> static_cast<unsigned>(l)) & 1u; }
  bool lamp_test_active() const { return p_.clock.now_ms() < lamp_test_until_; }
  // Battery as last reported (mV; under load for primary cells, 0 = unknown).
  std::uint32_t battery_mv() const { return battery_mv_; }
  // Cells below BoardSupport::battery_low_mv, or missing (BATT lamp).
  bool battery_low() const { return battery_mv_ < p_.board.battery_low_mv; }
  // True while a built-in sensor is held off after a fault (bit n = slot n).
  std::uint8_t fixed_quarantined() const { return fixed_quarantined_; }
  // Is slot i a built-in module (BoardSupport SlotWiring::fixed_module)?
  bool fixed(std::uint8_t i) const { return i < kSlots && p_.board.slots[i].fixed_module != 0; }
  // Debounced enclosure tamper loop: true = open or cut, nullopt = not known yet.
  std::optional<bool> tamper_open() const { return tamper_.state(); }
  // EXP_WAKE held high after servicing every expander: masked as a wake source.
  bool expander_wake_masked() const { return exp_wake_masked_; }
  // Tamper causes as reported to the hub (HD-049): the lid (enclosure loop) and
  // the battery door; the magnet-defeat and mount-pose detectors.
  bool lid_open_reported() const { return lid_reported_; }
  bool battery_door_open_reported() const { return door_reported_; }
  bool magnet_defeated() const { return magnet_.defeated(); }
  bool mount_moved() const { return mount_.moved(); }
  const std::optional<Vec3>& mount_baseline() const { return mount_.baseline(); }
  // Radio power cap active (primary cells below battery_low_mv, HD-043).
  bool tx_capped() const { return tx_capped_; }
  // Sleep design in force: the hub's ConfigUpdate, else the board default.
  PowerProfile power_profile() const;
  // What the platform should do until next_wake_ms() (sleep_policy.h).
  // console_active: a USB host used the console recently (the platform knows).
  SleepPlan sleep_plan(bool console_active = false) const;

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
  PowerMode compute_mode(std::uint32_t battery_mv);
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
  void update_lamps(TimeMs now);
  void poll_service_button(TimeMs now);
  void poll_user_button(TimeMs now);
  void start_lamp_test(TimeMs now);
  void report_console_refusals(TimeMs now);
  std::optional<bool> tamper_raw();
  // Splits the debounced loop into lid and battery door and reports each change.
  void update_tamper_causes(TimeMs now, bool first);
  // Runs MagnetDefeat on slot i's newest field reading (door closed only).
  void check_magnet(std::uint8_t i);
  // Reads gravity from slot i's LIS2DW12: re-baselines when due, else checks the pose.
  void check_mount(std::uint8_t i, TimeMs now);
  void apply_tx_power();
  // Direct deep-sleep wake lines (TL-SN-002 tamper and USER): armed only while
  // they read idle, so a held button or an open lid can't re-wake the node forever.
  void arm_direct_wake_lines();
  // Battery for heartbeats and the power mode; primary cells: lowest since the last call.
  std::uint32_t take_battery_mv();
  void sample_battery_under_load();
  void update_power_mode(std::uint32_t mv);
  void quarantine_fixed(std::uint8_t i, ModuleFault why, TimeMs now);
  TimeMs fixed_backoff_ms(std::uint8_t i) const;
  std::array<SlotLedMode, kSlots> slot_led_modes() const;
  StatusInputs status_inputs(TimeMs now) const;
  void arm_wake_lines();
  int send_due(TimeMs now);
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

  // TL-SN-002: USER button, board lamps, battery under load, built-in sensor quarantine.
  UserButton user_;
  TimeMs lamp_test_started_ = 0;
  TimeMs lamp_test_until_ = 0;
  std::uint8_t lamp_shadow_ = 0;
  bool lamps_known_ = false;
  TimeMs fault_lamp_until_ = 0;
  std::uint32_t battery_mv_ = 0;
  std::uint32_t loaded_mv_ = ~std::uint32_t{0};
  std::uint8_t fixed_quarantined_ = 0;
  std::uint8_t quarantine_pending_ = 0;  // set inside emit(), applied after the driver returns
  std::array<ModuleFault, kSlots> quarantine_why_{};
  std::array<TimeMs, kSlots> fixed_retry_at_{};
  std::array<std::uint8_t, kSlots> fixed_failures_{};
  std::uint32_t direct_wake_armed_ = 0;  // bit n: GPIO n armed by arm_direct_wake_lines()
  bool direct_wake_known_ = false;
  TimeMs last_user_report_ms_ = kNever;
  TimeMs last_user_commission_ms_ = kNever;
  // Direct tamper line reported open: it is masked as a wake source (ANY_HIGH
  // would re-wake forever), so the node polls it this often to see the lid
  // closed and, above all, opened again (red team: a close and a re-open inside
  // one sleep were invisible until the next heartbeat).
  TimeMs tamper_poll_at_ = kNever;
  bool lid_reported_ = false;
  bool door_reported_ = false;
  MagnetDefeat magnet_;
  std::uint32_t magnet_seq_seen_ = 0;
  bool magnet_was_open_ = true;
  MountPose mount_;
  TimeMs mount_rebaseline_at_ = kNever;
  TimeMs mount_check_at_ = kNever;  // pose check due (kMountAfterImpactMs after an impact)
  bool tx_capped_ = false;
  std::int8_t tx_applied_ = 0x7F;

  // USB console lock: refusals since boot, not yet reported, and the last report.
  std::uint32_t console_refused_total_ = 0;
  std::uint32_t console_unreported_ = 0;
  TimeMs console_reported_at_ = kNever;

  // Wake-line supervision.
  std::array<TimeMs, kSlots> wake_high_since_{};
  std::uint8_t wake_masked_ = 0;
  std::uint8_t wake_stuck_reported_ = 0;
  std::array<TimeMs, kSlots> next_masked_poll_{};
};

}  // namespace talos::fw
