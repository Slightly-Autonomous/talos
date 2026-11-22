#include "firmware/portable/runtime.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <type_traits>

namespace talos::fw {

namespace {

template <typename T>
bool load(IStore& store, StoreKey key, T& out) {
  static_assert(std::is_trivially_copyable_v<T>);
  T tmp;
  auto n = store.get(key, reinterpret_cast<std::uint8_t*>(&tmp), sizeof tmp);
  if (!n || *n != sizeof tmp) return false;
  out = tmp;
  return true;
}

template <typename T>
bool save(IStore& store, StoreKey key, const T& v) {
  static_assert(std::is_trivially_copyable_v<T>);
  return store.put(key, reinterpret_cast<const std::uint8_t*>(&v), sizeof v);
}

struct SlotPowerBlob {
  std::uint8_t powering = NodeRuntime::kNotPowering;  // slot whose AON was being switched on
  std::uint8_t quarantined = 0;                       // bit n: slot n stays unpowered
  std::array<std::uint8_t, kSlots> strikes{};         // consecutive resets while powering slot n
};

// The last epoch is never handed out: reaching it means the counter is used up.
constexpr std::uint32_t kEpochExhausted = std::numeric_limits<std::uint32_t>::max();

TimeMs quarantine_backoff_ms(std::uint8_t strikes) {
  const unsigned extra = strikes > NodeRuntime::kQuarantineStrikes
                             ? static_cast<unsigned>(strikes - NodeRuntime::kQuarantineStrikes)
                             : 0u;
  TimeMs ms = NodeRuntime::kQuarantineRetryMs;
  for (unsigned i = 0; i < extra && ms < NodeRuntime::kQuarantineRetryMaxMs; ++i) ms *= 2;
  return ms < NodeRuntime::kQuarantineRetryMaxMs ? ms : NodeRuntime::kQuarantineRetryMaxMs;
}

// One unacknowledged P0 per ring slot (~44 B, about 4 NVS entries) instead of
// rewriting all of them (~328 B, 13 entries) on every alarm and every ACK.
// Each record also snapshots which slots are free (empty or acknowledged), so
// the newest record (highest gen) or ACK record tells the whole ring's state.
struct PendingRecord {
  std::uint32_t gen = 0;
  std::uint8_t free_mask = 0;
  std::uint8_t reserved[3] = {};
  DeviceEvent event;
};
struct PendingAcks {
  std::uint32_t gen = 0;
  std::uint8_t free_mask = 0;
  std::uint8_t reserved[3] = {};
};

constexpr std::size_t kRing = NodeRuntime::kMaxPendingCritical;
static_assert(static_cast<std::size_t>(StoreKey::kPendingRingLast) -
                      static_cast<std::size_t>(StoreKey::kPendingRing0) + 1 ==
                  kRing,
              "one store key per ring slot");
static_assert(kRing <= 8, "ring masks are 8 bits");

StoreKey ring_key(std::size_t i) {
  return static_cast<StoreKey>(static_cast<std::size_t>(StoreKey::kPendingRing0) + i);
}

// Runtime state kept in LP RAM across an ESP32-H2 deep sleep (which is a reset).
// Only trusted on a deep-sleep wake, and only with the magic and CRC intact.
constexpr std::uint32_t kRetainedMagic = 0x544C5333u;  // "TLS3" (TLS2 had no tamper causes)
struct RetainedState {
  std::uint32_t magic;
  std::uint32_t boot_epoch;  // already persisted: a wake never writes NVS for it
  std::uint32_t seq;         // so (epoch, seq) is never reused across the wake
  std::uint32_t heartbeat_in_ms;
  std::uint32_t probe_backoff_ms;
  std::array<std::uint32_t, kRing> ring_gen;
  std::array<std::uint16_t, kSlots> code;  // module running in each slot (0 = none)
  std::array<std::uint8_t, kSlots> state;  // its driver's retained_state()
  std::uint8_t aon_on;
  std::uint8_t wake_masked;
  std::uint8_t ring_free;
  // bit 0 link failing, bit 1 ACKs not yet in NVS, bit 2 tamper state known,
  // bit 3 tamper open, bit 4 EXP_WAKE masked as a wake source.
  std::uint8_t flags;
  // Built-in sensors (ADR 0019): held off after a fault, their failure counts and
  // when each is retried (relative, like heartbeat_in_ms). Without these a node
  // that wakes more often than the retry period would never retry (red team).
  std::uint8_t fixed_quarantined;
  std::array<std::uint8_t, kSlots> fixed_failures;
  std::uint8_t power_mode;  // PowerMode: the hysteresis needs the mode it leaves
  // Primary cells: the battery level under load and the lowest reading since the
  // last heartbeat (0xFFFF = none yet). A wake must not fall back to the resting
  // voltage, which hides an empty Li-FeS2 pair (red team).
  std::uint16_t battery_mv;
  std::uint16_t loaded_mv;
  std::array<std::uint32_t, kSlots> fixed_retry_in_ms;
  // Tamper causes (HD-049): bit 0 lid reported open, bit 1 battery door reported
  // open, bit 2 magnet baseline valid, bit 3 magnet defeated, bit 4 mount moved,
  // bit 5 door last read open (magnet), bit 6 mount re-baseline pending.
  std::uint8_t flags2;
  std::uint8_t reserved2;
  std::array<std::int16_t, 3> magnet_base;  // closed-door field, units of 100 uT
  std::uint32_t crc;
};
static_assert(std::is_trivially_copyable_v<RetainedState>);
static_assert(sizeof(RetainedState) <= ISleep::kMinRetainedBytes,
              "LP RAM block too big: every deep-sleep wake would become a cold boot");

std::uint32_t retained_crc(const RetainedState& rs) {
  return crc8(reinterpret_cast<const std::uint8_t*>(&rs), offsetof(RetainedState, crc));
}

bool read_retained(ISleep* sleep, RetainedState& rs) {
  if (!sleep || !sleep->retained() || sleep->retained_size() < sizeof rs) return false;
  std::memcpy(&rs, sleep->retained(), sizeof rs);
  return rs.magic == kRetainedMagic && rs.crc == retained_crc(rs) && rs.boot_epoch != 0 &&
         rs.boot_epoch != kEpochExhausted;
}

// Faults that mean a sensor stopped watching: P0 on a critical slot.
bool is_supervision_fault(std::int32_t a, std::uint32_t b) {
  switch (static_cast<ModuleFault>(a)) {
    case ModuleFault::kStuckInterrupt:
    case ModuleFault::kPowerQuarantined:
    case ModuleFault::kCardRemoved:
    case ModuleFault::kWakeLineFault:
      return true;
    case ModuleFault::kBusError:
      return b == kFaultDetailUnarmed;
    default:
      return false;
  }
}

std::uint32_t clamp_interval(std::uint32_t v, std::uint32_t max) {
  return std::clamp(v, NodeRuntime::kMinIntervalMs, max);
}

}  // namespace

bool NodeRuntime::slot_critical(std::uint8_t slot) const {
  if (slot >= kSlots) return false;
  const ModuleInfo* info = find_module(slots_[slot].code);
  return info ? info->critical : true;  // unknown (e.g. quarantined before its ID was read)
}

void NodeRuntime::emit(std::uint8_t slot, EventKind kind, Priority priority, bool flag,
                       std::int32_t a, std::uint32_t b) {
  // A built-in sensor can't be reseated: one that fails to start is lost sensing too.
  const bool fixed_lost = fixed(slot) && static_cast<ModuleFault>(a) == ModuleFault::kInitFailed;
  if (kind == EventKind::kModuleFault && priority != Priority::kP0Critical &&
      (is_supervision_fault(a, b) || fixed_lost) && slot_critical(slot)) {
    priority = Priority::kP0Critical;  // intrusion sensing is lost: a trouble alarm
  }
  if (kind == EventKind::kModuleFault) {
    fault_lamp_until_ = p_.clock.now_ms() + kFaultLampMs;
    // A built-in sensor stuck in warm-up or with its line stuck high is
    // quarantined (disabled, PIR rail off) once the driver has returned.
    const auto f = static_cast<ModuleFault>(a);
    if (fixed(slot) && (f == ModuleFault::kWarmupTimeout || f == ModuleFault::kStuckInterrupt)) {
      quarantine_pending_ = static_cast<std::uint8_t>(quarantine_pending_ | (1u << slot));
      quarantine_why_[slot] = f;
    }
  }
  DeviceEvent e;
  e.boot_epoch = boot_epoch_;
  e.seq = ++seq_;
  e.slot = slot;
  e.kind = kind;
  e.priority = priority;
  e.at_ms = p_.clock.now_ms();
  e.flag = flag;
  e.a = a;
  e.b = b;
  if (kind == EventKind::kHeartbeat) {
    // Only the newest heartbeat matters: an outage never queues a backlog of them.
    queue_.remove_if([](const DeviceEvent& q) { return q.kind == EventKind::kHeartbeat; });
  }
  queue_.push(e);
  if (priority == Priority::kP0Critical) {
    persist_critical(e);
    p0_waiting_ = true;
  }
  // An impact may be someone prying the node off its mount: check the pose once
  // the jolt has passed (a reading during it is not gravity).
  if (kind == EventKind::kImpact && slot < kSlots && mount_check_at_ == kNever) {
    mount_check_at_ = p_.clock.now_ms() + kMountAfterImpactMs;
  }
  // L-03: a wake event from a slot is a short flash of its LED (when lit at all).
  if (slot < kSlots &&
      (kind == EventKind::kMotion || kind == EventKind::kContact || kind == EventKind::kImpact ||
       (kind == EventKind::kEnvironment && fixed(slot)))) {  // TL-SN-002 ENV lamp: per reading
    slot_leds_.note_wake(slot, e.at_ms);
  }
}

void NodeRuntime::persist_critical(const DeviceEvent& e) {
  // A free slot if there is one, else the oldest record: the newest alarms survive.
  std::size_t pick = kRing;
  for (std::size_t i = 0; i < kRing; ++i) {
    if ((ring_free_ & (1u << i)) && (pick == kRing || ring_gen_[i] < ring_gen_[pick])) pick = i;
  }
  if (pick == kRing) {
    pick = 0;
    for (std::size_t i = 1; i < kRing; ++i) {
      if (ring_gen_[i] < ring_gen_[pick]) pick = i;
    }
  }
  PendingRecord r;
  r.gen = next_gen_++;
  r.free_mask = static_cast<std::uint8_t>(ring_free_ & ~(1u << pick));
  r.event = e;
  if (!save(p_.store, ring_key(pick), r)) return;  // still queued in RAM
  ring_gen_[pick] = r.gen;
  ring_epoch_[pick] = e.boot_epoch;
  ring_seq_[pick] = e.seq;
  ring_free_ = r.free_mask;
  acks_dirty_ = false;  // the record carried every ACK so far
}

void NodeRuntime::note_ack(std::uint32_t boot_epoch, std::uint32_t seq, TimeMs now) {
  for (std::size_t i = 0; i < kRing; ++i) {
    const auto bit = static_cast<std::uint8_t>(1u << i);
    if (!(ring_free_ & bit) && ring_epoch_[i] == boot_epoch && ring_seq_[i] == seq) {
      ring_free_ = static_cast<std::uint8_t>(ring_free_ | bit);
      if (!acks_dirty_) acks_dirty_since_ = now;
      acks_dirty_ = true;
    }
  }
}

// Written back lazily (kAckFlushMs, or before a planned reboot): an ACK lost to
// a power cut only means the hub sees that alarm again and deduplicates it.
void NodeRuntime::flush_acks() {
  if (!acks_dirty_) return;
  PendingAcks a;
  a.gen = next_gen_++;
  a.free_mask = ring_free_;
  if (save(p_.store, StoreKey::kPendingAcks, a)) acks_dirty_ = false;
}

void NodeRuntime::restore_pending_critical(
    std::uint8_t retained_free,
    const std::array<std::uint32_t, kMaxPendingCritical>* retained_gen) {
  std::array<PendingRecord, kRing> rec{};
  std::uint8_t have = 0;
  std::uint32_t best = 0;
  std::uint8_t free_mask = 0xFF;
  for (std::size_t i = 0; i < kRing; ++i) {
    if (!load(p_.store, ring_key(i), rec[i]) || rec[i].gen == 0) continue;
    have = static_cast<std::uint8_t>(have | (1u << i));
    ring_gen_[i] = rec[i].gen;
    ring_epoch_[i] = rec[i].event.boot_epoch;
    ring_seq_[i] = rec[i].event.seq;
    if (rec[i].gen > best) {
      best = rec[i].gen;
      free_mask = rec[i].free_mask;
    }
  }
  PendingAcks acks;
  if (load(p_.store, StoreKey::kPendingAcks, acks) && acks.gen > best) {
    best = acks.gen;
    free_mask = acks.free_mask;
  }
  next_gen_ = best + 1;
  free_mask = static_cast<std::uint8_t>(free_mask | ~have);  // never written: free
  if (retained_gen) {
    for (std::size_t i = 0; i < kRing; ++i) {
      if ((retained_free & (1u << i)) && (*retained_gen)[i] == ring_gen_[i]) {
        free_mask = static_cast<std::uint8_t>(free_mask | (1u << i));
      }
    }
  }
  ring_free_ = free_mask;
  // Oldest first, each with its original (boot_epoch, seq): the hub deduplicates.
  std::uint8_t todo = static_cast<std::uint8_t>(have & ~free_mask);
  while (todo) {
    std::size_t next = kRing;
    for (std::size_t i = 0; i < kRing; ++i) {
      if ((todo & (1u << i)) && (next == kRing || ring_gen_[i] < ring_gen_[next])) next = i;
    }
    queue_.push(rec[next].event);
    todo = static_cast<std::uint8_t>(todo & ~(1u << next));
  }
}

bool NodeRuntime::persist_boot_epoch() {
  if (boot_epoch_ == kEpochExhausted) return false;  // fail loudly: never send again
  return save(p_.store, StoreKey::kBootEpoch, boot_epoch_);
}

void NodeRuntime::save_retained() {
  if (!sleep_ || !sleep_->retained() || sleep_->retained_size() < sizeof(RetainedState)) return;
  RetainedState rs;
  std::memset(&rs, 0, sizeof rs);
  // An epoch that is not on record yet must not survive a wake (it would be
  // used without ever being persisted): leave the block invalid instead.
  if (epoch_persisted_ && boot_epoch_ != kEpochExhausted) {
    const TimeMs now = p_.clock.now_ms();
    rs.magic = kRetainedMagic;
    rs.boot_epoch = boot_epoch_;
    rs.seq = seq_;
    const TimeMs hb = next_heartbeat_ > now ? next_heartbeat_ - now : 0;
    rs.heartbeat_in_ms = static_cast<std::uint32_t>(std::min<TimeMs>(hb, kMaxHeartbeatIntervalMs));
    rs.probe_backoff_ms = probe_backoff_ms_;
    rs.ring_gen = ring_gen_;
    for (std::uint8_t i = 0; i < kSlots; ++i) {
      const SlotState& st = slots_[i];
      if (!st.enabled) continue;
      rs.code[i] = static_cast<std::uint16_t>(st.code);
      rs.state[i] = std::visit(
          [](const auto& drv) -> std::uint8_t {
            if constexpr (requires { drv.retained_state(); }) {
              return drv.retained_state();
            } else {
              return 0;
            }
          },
          st.driver);
    }
    rs.aon_on = aon_on_;
    rs.wake_masked = wake_masked_;
    rs.ring_free = ring_free_;
    const auto tamper = tamper_.state();
    rs.flags = static_cast<std::uint8_t>((outage_ ? 1u : 0u) | (acks_dirty_ ? 2u : 0u) |
                                         (tamper ? 4u : 0u) | (tamper.value_or(false) ? 8u : 0u) |
                                         (exp_wake_masked_ ? 16u : 0u));
    rs.fixed_quarantined = fixed_quarantined_;
    rs.fixed_failures = fixed_failures_;
    rs.power_mode = static_cast<std::uint8_t>(mode_);
    for (std::uint8_t i = 0; i < kSlots; ++i) {
      const TimeMs in = fixed_retry_at_[i] > now ? fixed_retry_at_[i] - now : 0;
      rs.fixed_retry_in_ms[i] = static_cast<std::uint32_t>(std::min<TimeMs>(in, kFixedRetryMaxMs));
    }
    rs.battery_mv = static_cast<std::uint16_t>(std::min<std::uint32_t>(battery_mv_, 0xFFFE));
    rs.loaded_mv = static_cast<std::uint16_t>(std::min<std::uint32_t>(loaded_mv_, 0xFFFF));
    const auto& mb = magnet_.baseline();
    rs.flags2 = static_cast<std::uint8_t>(
        (lid_reported_ ? 1u : 0u) | (door_reported_ ? 2u : 0u) | (mb ? 4u : 0u) |
        (magnet_.defeated() ? 8u : 0u) | (mount_.moved() ? 16u : 0u) |
        (magnet_was_open_ ? 32u : 0u) | (mount_rebaseline_at_ != kNever ? 64u : 0u));
    if (mb) {
      auto q = [](std::int32_t ut) {
        return static_cast<std::int16_t>(std::clamp<std::int32_t>(ut / 100, -32767, 32767));
      };
      rs.magnet_base = {q(mb->x), q(mb->y), q(mb->z)};
    }
    rs.crc = retained_crc(rs);
  }
  std::memcpy(sleep_->retained(), &rs, sizeof rs);
}

void NodeRuntime::boot() {
  feed_watchdog();
  wake_ = sleep_ ? sleep_->wake_info() : WakeInfo{};
  RetainedState rs;
  std::memset(&rs, 0, sizeof rs);
  const bool warm = wake_.deep_sleep() && read_retained(sleep_, rs);
  if (warm) {
    // Deep-sleep wake: same epoch, sequence continues. No NVS write per wake.
    boot_epoch_ = rs.boot_epoch;
    seq_ = rs.seq;
    epoch_persisted_ = true;
  } else {
    // Persistent boot epoch: strictly increasing, never reuse (epoch, seq) after a
    // reset. Until it is written nothing is transmitted (step() retries).
    std::uint32_t epoch = 0;
    load(p_.store, StoreKey::kBootEpoch, epoch);
    boot_epoch_ = epoch >= kEpochExhausted - 1 ? kEpochExhausted : epoch + 1;
    epoch_persisted_ = persist_boot_epoch() || persist_boot_epoch();
    seq_ = 0;
  }
  next_epoch_retry_ = p_.clock.now_ms() + kEpochRetryMs;
  load(p_.store, StoreKey::kConfig, config_);
  // A record from before the bounds existed (red team RT-14) is repaired, not obeyed.
  if (config_.revision == std::numeric_limits<std::uint32_t>::max()) config_.revision = 0;
  config_.heartbeat_interval_ms =
      clamp_interval(config_.heartbeat_interval_ms, kMaxHeartbeatIntervalMs);
  config_.environment_interval_ms =
      clamp_interval(config_.environment_interval_ms, kMaxEnvironmentIntervalMs);

  const BootDecision d = ota_.boot();
  restore_pending_critical(warm ? rs.ring_free : 0, warm ? &rs.ring_gen : nullptr);
  const TimeMs t0 = p_.clock.now_ms();
  boot_ms_ = t0;
  if (warm && (rs.flags & 2u)) {
    acks_dirty_ = true;
    acks_dirty_since_ = t0;
  }
  // Cards keep running through a deep sleep (AON on the expanders): adopt their
  // rails instead of cutting them, or fall back to the cold-boot safe state.
  resumed_cards_ = warm && p_.power.resume(rs.aon_on, board_ok_);
  if (resumed_cards_) {
    aon_on_ = rs.aon_on;
  } else {
    board_ok_ = p_.power.begin();  // safe outputs before anything touches a slot
    aon_on_ = 0;
  }
  // A reset while a slot's AON was being switched on may mean that card pulled
  // the core rail down (no current limit on AON). It may also be a USB unplug
  // or a watchdog: one is a strike, kQuarantineStrikes in a row keep the slot
  // off instead of boot-looping, and the quarantine is retried with backoff.
  SlotPowerBlob sp;
  load(p_.store, StoreKey::kSlotPower, sp);
  if (sp.powering < kSlots) {
    auto& n = sp.strikes[sp.powering];
    if (n < 0xFF) ++n;
    if (n >= kQuarantineStrikes) {
      sp.quarantined = static_cast<std::uint8_t>(sp.quarantined | (1u << sp.powering));
    }
    sp.powering = kNotPowering;
    save(p_.store, StoreKey::kSlotPower, sp);
  }
  quarantined_ = sp.quarantined;
  strikes_ = sp.strikes;
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    quarantine_retry_at_[i] = t0 + quarantine_backoff_ms(strikes_[i]);
    wake_high_since_[i] = kNever;
  }
  wake_masked_ = resumed_cards_ ? rs.wake_masked : 0;
  wake_stuck_reported_ = 0;
  // Link health: a wake keeps rationing sends if the hub was unreachable.
  last_reply_ms_ = t0;
  last_refill_ms_ = t0;
  tokens_ = kSendBurst;
  outage_ = warm && (rs.flags & 1u);
  unanswered_ = outage_ ? kLinkFailSends : 0;
  probe_backoff_ms_ =
      outage_ ? std::clamp(rs.probe_backoff_ms, kProbeMinMs, kProbeMaxMs) : kProbeMinMs;
  next_probe_ms_ = t0;
  // Indicators: a wake keeps "joined" if the hub was answering before the sleep.
  hub_heard_ = warm && !outage_;
  tamper_ = TamperDebouncer{};
  button_ = ServiceButton{};
  user_ = UserButton{};
  // The USER press that woke the node counts from now (a cold boot ignores a held button).
  if (warm && p_.board.user_button_line < 32 && (wake_.lines & (1u << p_.board.user_button_line))) {
    user_.begin_held(t0);
  }
  lamp_test_until_ = 0;
  lamp_shadow_ = 0;
  lamps_known_ = !resumed_cards_;  // begin() wrote every lamp dark
  fault_lamp_until_ = 0;
  // Primary cells: keep the loaded minimum across a deep sleep (a wake that read
  // only the resting voltage would report a full-looking empty pair).
  loaded_mv_ = warm && rs.loaded_mv != 0xFFFF ? rs.loaded_mv : ~std::uint32_t{0};
  direct_wake_known_ = false;
  quarantine_pending_ = 0;
  tamper_poll_at_ = kNever;
  last_user_report_ms_ = kNever;
  last_user_commission_ms_ = kNever;
  fixed_quarantined_ = warm ? rs.fixed_quarantined : 0;
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    fixed_failures_[i] = warm ? rs.fixed_failures[i] : 0;
    fixed_retry_at_[i] = t0 + (warm ? std::min<TimeMs>(rs.fixed_retry_in_ms[i], kFixedRetryMaxMs)
                                    : TimeMs{kFixedRetryMs});
  }
  if (warm && (rs.flags & 4u)) tamper_.restore((rs.flags & 8u) != 0);
  lid_reported_ = warm && (rs.flags2 & 1u);
  door_reported_ = warm && (rs.flags2 & 2u);
  magnet_was_open_ = !warm || (rs.flags2 & 32u);
  magnet_seq_seen_ = 0;
  if (warm && (rs.flags2 & 4u)) {
    magnet_.restore(Vec3{rs.magnet_base[0] * 100, rs.magnet_base[1] * 100, rs.magnet_base[2] * 100},
                    (rs.flags2 & 8u) != 0);
  } else {
    magnet_.restore(std::nullopt, false);
  }
  {
    std::array<std::int32_t, 3> g{};
    const bool have = load(p_.store, StoreKey::kMountBaseline, g);
    mount_.restore(have ? std::optional<Vec3>(Vec3{g[0], g[1], g[2]}) : std::nullopt,
                   warm && (rs.flags2 & 16u));
  }
  // A pending re-baseline survives the wake (it is due a few seconds after a close).
  mount_rebaseline_at_ = warm && (rs.flags2 & 64u) ? t0 : kNever;
  mount_check_at_ = kNever;
  tx_capped_ = false;
  tx_applied_ = 0x7F;
  exp_wake_masked_ = warm && (rs.flags & 16u);
  exp_wake_high_ = false;
  next_exp_poll_ = t0;
  status_ = StatusCode::kOff;
  status_led_known_ = false;
  slot_led_shadow_ = 0;
  slot_leds_known_ = !resumed_cards_;  // begin() wrote every slot LED dark
  if (warm && p_.board.primary_battery) {
    // The level under load from before the sleep stands until the next heartbeat;
    // a fresh reading only ever lowers it (as between heartbeats).
    battery_mv_ = std::min<std::uint32_t>(rs.battery_mv, p_.power.battery_mv());
  } else {
    battery_mv_ = take_battery_mv();
  }
  mode_ = warm && rs.power_mode <= static_cast<std::uint8_t>(PowerMode::kCritical)
              ? static_cast<PowerMode>(rs.power_mode)
              : PowerMode::kNormal;
  mode_ = compute_mode(battery_mv_);
  Resume r;
  if (resumed_cards_) {
    r.active = true;
    r.code = rs.code;
    r.state = rs.state;
  }
  discover(r);
  admit_power();
  refresh_external_power();
  apply_tx_power();
  for (std::uint8_t i = 0; i < kSlots; ++i) check_magnet(i);  // baseline a door found closed
  // EXP_WAKE first: on a wake from it this reads TAMPER_N and releases INT.
  service_expanders(p_.clock.now_ms());
  sample_tamper(p_.clock.now_ms());
  if (p_.board.tamper_line != 0xFF && tamper_.state() == std::optional<bool>{true}) {
    tamper_poll_at_ = p_.clock.now_ms() + kMaskedPollMs;
  }
  poll_user_button(p_.clock.now_ms());
  arm_wake_lines();

  const TimeMs now = p_.clock.now_ms();
  if (warm) {
    // The hub already has the version and the health; a timer wake is the
    // heartbeat (or a probe) coming due, a GPIO wake keeps the schedule.
    const TimeMs left = wake_.cause == ResetCause::kDeepSleepTimer
                            ? 0
                            : std::min<TimeMs>(rs.heartbeat_in_ms, config_.heartbeat_interval_ms);
    next_heartbeat_ = now + left;
  } else {
    // Boot status tells the hub the running version (and any rollback).
    (void)d;
    emit_ota_status(Priority::kP1State);
    // Why the chip started: a reset through the outside USB-C port with the
    // enclosure shut is worth seeing on the hub (owner 2026-11-22).
    emit(kNoSlot, EventKind::kReset, Priority::kP1State, false,
         static_cast<std::int32_t>(wake_.cause),
         tamper_raw() == std::optional<bool>{false} ? 1u : 0u);
    emit(kNoSlot, EventKind::kHeartbeat, Priority::kP2Health, false,
         static_cast<std::int32_t>(battery_mv_), static_cast<std::uint32_t>(mode_));
    next_heartbeat_ = now + config_.heartbeat_interval_ms;
  }
  update_indicators(now);
  save_retained();
}

void NodeRuntime::emit_ota_status(Priority priority) {
  const OtaRecord& r = ota_.record();
  // b: next wanted byte while downloading, otherwise the running version.
  const std::uint32_t b = r.state == OtaState::kDownloading ? r.received : ota_.running_version();
  emit(kNoSlot, EventKind::kOtaStatus, priority, false, static_cast<std::int32_t>(r.state), b);
}

void NodeRuntime::discover(const Resume& r) {
  const TimeMs now = p_.clock.now_ms();
  std::array<bool, kSlots> done{};
  auto run = [&](std::uint8_t i) {
    discover_slot(i, now, r.active ? &r : nullptr);
    done[i] = true;
    feed_watchdog();  // each slot can take tens of ms (rail settle, NVS, sensor probes)
  };
  // After a wake, the slots that woke the node first: their event is the alarm.
  if (r.active) {
    for (std::uint8_t i = 0; i < kSlots; ++i) {
      const std::uint8_t line = p_.board.slots[i].wake_line;
      if (line < 32 && (wake_.lines & (1u << line))) run(i);
    }
  }
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    if (!done[i]) run(i);
  }
}

void NodeRuntime::rescan(std::optional<std::uint8_t> slot) {
  const TimeMs now = p_.clock.now_ms();
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    if (!slot || *slot == i) {
      discover_slot(i, now);
      feed_watchdog();
    }
  }
  admit_power();
  arm_wake_lines();
}

void NodeRuntime::discover_slot(std::uint8_t i, TimeMs now, const Resume* r) {
  SlotState& st = slots_[i];
  st = SlotState{};
  const SlotWiring& w = p_.board.slots[i];
  if (!w.present) return;
  if (fixed(i) && (fixed_quarantined_ & (1u << i))) {
    // Held off after a fault (state from before a deep sleep): retried on schedule.
    st.code = static_cast<ModuleCode>(w.fixed_module);
    st.fault = ModuleFault::kInitFailed;
    unpower_slot(i);
    return;
  }
  if (!power_slot(i)) return;
  bool keep_powered = true;
  {
    SlotIo io(p_, i);
    keep_powered = init_slot(i, io, now, r);
    io.deselect();  // leave the root bus free, and never unpower a selected branch
  }
  // A card that can't be used is not left drawing from AON (ADR 0013: unknown
  // codes are never powered). Rescan powers it again.
  if (!keep_powered) unpower_slot(i);
}

void NodeRuntime::unpower_slot(std::uint8_t i) {
  if (!p_.board.slot_io[i].aon_enable.wired()) return;
  p_.power.set_rail(i, Rail::kAlwaysOn3v3, false);
  aon_on_ = static_cast<std::uint8_t>(aon_on_ & ~(1u << i));
}

bool NodeRuntime::init_slot(std::uint8_t i, SlotIo& io, TimeMs now, const Resume* r) {
  SlotState& st = slots_[i];
  const SlotWiring& w = p_.board.slots[i];
  // A built-in module (ADR 0019) has no ID EEPROM and no presence line: it is
  // whatever the board table says, from boot.
  IdRead id;
  if (fixed(i)) {
    id.status = IdRead::Status::kOk;
    id.code = static_cast<ModuleCode>(w.fixed_module);
  } else {
    id = read_module_id(io, p_.board.id_eeprom_address);
  }
  st.code = id.code;
  auto fault = [&](ModuleFault f) {
    st.fault = f;
    if (fixed(i)) {
      fixed_retry_at_[i] = now + fixed_backoff_ms(i);
      if (fixed_failures_[i] < 0xFF) ++fixed_failures_[i];
    }
    emit(i, EventKind::kModuleFault, Priority::kP2Health, false, static_cast<std::int32_t>(f),
         static_cast<std::uint32_t>(st.code));
  };
  const std::optional<bool> seated =
      fixed(i) ? std::optional<bool>{true} : p_.power.slot_present(i);
  switch (id.status) {
    case IdRead::Status::kAbsent:
      // A seated card whose ID does not answer is a fault, not an empty slot.
      // It stays on AON so the bench can still scan the card's other devices.
      if (seated.value_or(false)) {
        fault(ModuleFault::kNoIdEeprom);
        return true;
      }
      return false;
    case IdRead::Status::kMuxFault:
      fault(ModuleFault::kMuxFault);  // the mux, not the card: leave the card alone
      return true;
    case IdRead::Status::kBlank:
      fault(ModuleFault::kIdBlank);  // AON goes off again
      return false;
    case IdRead::Status::kCorrupt:
      fault(ModuleFault::kIdCorrupt);
      return false;
    case IdRead::Status::kOk:
      break;
  }
  const ModuleInfo* info = find_module(st.code);
  if (!info) {
    fault(ModuleFault::kUnknownModule);  // unknown module: never left powered
    return false;
  }
  if ((info->needs_wake && w.wake_line == 0xFF) || (info->needs_5v && !w.has_5v)) {
    fault(ModuleFault::kMissingResource);
    return false;
  }
  // Same card as before a deep sleep, still powered: resume, never re-init.
  const bool resume = r && r->code[i] == static_cast<std::uint16_t>(st.code);
  const std::uint8_t rs = resume ? r->state[i] : 0;
  bool ok = false;
  switch (st.code) {
    case ModuleCode::kPirEkmb:
    case ModuleCode::kTlMod002Pir:
    case ModuleCode::kPir223d: {
      auto& d = st.driver.emplace<PirDriver>();
      if (st.code == ModuleCode::kPir223d) d.set_timing(PirDriver::kPir223d);
      ok = resume ? d.resume(io, *this, now, rs) : d.init(io, *this, now);
      break;
    }
    case ModuleCode::kTlTest001:
      ok = st.driver.emplace<TestCardDriver>().init(io, *this, now);
      break;
    case ModuleCode::kReedContact: {
      auto& d = st.driver.emplace<ContactDriver>();
      ok = resume ? d.resume(io, *this, now, rs) : d.init(io, *this, now);
      break;
    }
    case ModuleCode::kTmag5273Contact:
    case ModuleCode::kTmag5273A2: {
      auto& d = st.driver.emplace<Tmag5273ContactDriver>();
      if (st.code == ModuleCode::kTmag5273A2) {
        d.set_range(Tmag5273ContactDriver::kA2RangeMt, Tmag5273ContactDriver::kA2LsbPerMt);
      }
      ok = resume ? d.resume(io, *this, now, rs) : d.init(io, *this, now);
      break;
    }
    case ModuleCode::kLis2dw12:
    case ModuleCode::kTlMod003Imu: {
      auto& d = st.driver.emplace<Lis2dw12Driver>();
      ok = resume ? d.resume(io, *this, now, config_.impact_threshold_mg)
                  : d.init(io, *this, now, config_.impact_threshold_mg);
      break;
    }
    case ModuleCode::kSht40:
    case ModuleCode::kTlMod004Env:
      ok = st.driver.emplace<Sht40Driver>().init(io, *this, now);
      break;
    case ModuleCode::kSmartCameraS3:
      ok = st.driver.emplace<SmartModuleDriver>().init(io, *this, now);
      break;
    case ModuleCode::kNone:
      break;
  }
  if (!ok) {
    st.driver = std::monostate{};
    fault(ModuleFault::kInitFailed);
    return false;
  }
  st.enabled = true;
  if (fixed(i)) {
    fixed_failures_[i] = 0;
    fixed_quarantined_ = static_cast<std::uint8_t>(fixed_quarantined_ & ~(1u << i));
  }
  if (!resume) {
    emit(i, EventKind::kModulePresent, Priority::kP2Health, true,
         static_cast<std::int32_t>(st.code), 0);
  }
  return true;
}

TimeMs NodeRuntime::fixed_backoff_ms(std::uint8_t i) const {
  TimeMs ms = kFixedRetryMs;
  for (unsigned n = 0; n < fixed_failures_[i] && ms < kFixedRetryMaxMs; ++n) ms *= 2;
  return ms < kFixedRetryMaxMs ? ms : kFixedRetryMaxMs;
}

// Built-in sensors can't be reseated: a faulting one is disabled and reported,
// and retried with backoff. The PIR loses its rail (PIR_PWR), which is also how
// its retry power-cycles it; nothing else on TL-SN-002 has a rail to cycle.
void NodeRuntime::quarantine_fixed(std::uint8_t i, ModuleFault why, TimeMs now) {
  SlotState& st = slots_[i];
  if (!st.enabled) return;
  st.enabled = false;
  st.fault = why;
  st.driver = std::monostate{};
  unpower_slot(i);
  fixed_quarantined_ = static_cast<std::uint8_t>(fixed_quarantined_ | (1u << i));
  fixed_retry_at_[i] = now + fixed_backoff_ms(i);
  if (fixed_failures_[i] < 0xFF) ++fixed_failures_[i];
  arm_wake_lines();
}

void NodeRuntime::save_slot_power(std::uint8_t powering) {
  SlotPowerBlob sp;
  sp.powering = powering;
  sp.quarantined = quarantined_;
  sp.strikes = strikes_;
  save(p_.store, StoreKey::kSlotPower, sp);
}

void NodeRuntime::retry_quarantined(TimeMs now) {
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    const auto bit = static_cast<std::uint8_t>(1u << i);
    if (!(quarantined_ & bit) || now < quarantine_retry_at_[i]) continue;
    // Released in RAM only: power_slot() persists the attempt marker, so if the
    // card really shorts AON the next boot counts another strike and backs off further.
    quarantined_ = static_cast<std::uint8_t>(quarantined_ & ~bit);
    rescan(i);
    quarantine_retry_at_[i] = now + quarantine_backoff_ms(strikes_[i]);
  }
}

bool NodeRuntime::power_slot(std::uint8_t i) {
  // Boards without a switch on AON (or without one we can drive) have it always on.
  if (!p_.board.slot_io[i].aon_enable.wired()) return true;
  const auto bit = static_cast<std::uint8_t>(1u << i);
  if (quarantined_ & bit) {
    p_.power.set_rail(i, Rail::kAlwaysOn3v3, false);
    aon_on_ = static_cast<std::uint8_t>(aon_on_ & ~bit);
    slots_[i].fault = ModuleFault::kPowerQuarantined;
    emit(i, EventKind::kModuleFault, Priority::kP2Health, false,
         static_cast<std::int32_t>(ModuleFault::kPowerQuarantined), 0);
    return false;
  }
  // An empty slot stays off: no pull-up current, nothing to short.
  if (p_.power.slot_present(i) == std::optional<bool>{false}) {
    p_.power.set_rail(i, Rail::kAlwaysOn3v3, false);
    aon_on_ = static_cast<std::uint8_t>(aon_on_ & ~bit);
    return false;
  }
  if (aon_on_ & bit) {
    p_.power.set_rail(i, Rail::kAlwaysOn3v3, true);  // idempotent; a bench command may have cut it
    return true;
  }
  // Record the attempt first: if this card shorts AON the node resets right here,
  // and the next boot finds the marker and counts a strike against the slot.
  save_slot_power(i);
  p_.power.set_rail(i, Rail::kAlwaysOn3v3, true);
  p_.clock.delay_ms(kSlotPowerSettleMs);
  strikes_[i] = 0;  // it came up: the earlier resets were not this card
  save_slot_power(kNotPowering);
  aon_on_ = static_cast<std::uint8_t>(aon_on_ | bit);
  return true;
}

void NodeRuntime::clear_quarantine(std::uint8_t slot) {
  if (slot >= kSlots) return;
  if (!(quarantined_ & (1u << slot)) && strikes_[slot] == 0) return;
  quarantined_ = static_cast<std::uint8_t>(quarantined_ & ~(1u << slot));
  strikes_[slot] = 0;
  save_slot_power(kNotPowering);
}

std::uint32_t NodeRuntime::sustained_budget_mw() {
  switch (p_.power.source()) {
    case PowerSource::kUsb:
      return p_.board.usb_sustained_mw;
    case PowerSource::kSolarBattery:
      return p_.board.solar_sustained_mw;
    case PowerSource::kBatteryOnly:
    case PowerSource::kUnknown:  // can't tell: budget as if on battery
      break;
  }
  return p_.board.battery_sustained_mw;
}

// Critical sensing is always admitted; optional loads (camera) only when the
// active power source sustains them and the node is not in low power.
void NodeRuntime::admit_power() {
  std::uint32_t used = 0;
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    const ModuleInfo* info = find_module(slots_[i].code);
    if (info && info->critical && slots_[i].enabled) used += info->average_mw;
  }
  const std::uint32_t budget = sustained_budget_mw();
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    SlotState& st = slots_[i];
    const ModuleInfo* info = find_module(st.code);
    if (!info || info->critical || std::holds_alternative<std::monostate>(st.driver)) continue;
    const bool allow = mode_ == PowerMode::kNormal && used + info->average_mw <= budget;
    if (info->needs_5v) p_.power.set_rail(i, Rail::kSwitched5v, allow);
    if (allow) {
      used += info->average_mw;
      if (!st.enabled) {
        st.enabled = true;
        st.fault = ModuleFault::kNone;
      }
    } else if (st.enabled || st.fault != ModuleFault::kDisabledForPower) {
      st.enabled = false;
      st.fault = ModuleFault::kDisabledForPower;
      emit(i, EventKind::kModuleFault, Priority::kP2Health, false,
           static_cast<std::int32_t>(ModuleFault::kDisabledForPower),
           static_cast<std::uint32_t>(st.code));
    }
  }
}

// Every slot whose running module needs its wake line is a deep-sleep wake
// source, unless the line is masked as stuck high.
void NodeRuntime::arm_wake_lines() {
  if (!sleep_) return;
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    const std::uint8_t line = p_.board.slots[i].wake_line;
    if (line == 0xFF) continue;
    const ModuleInfo* info = find_module(slots_[i].code);
    const bool want = slots_[i].enabled && info && info->needs_wake && !(wake_masked_ & (1u << i));
    sleep_->set_wake_enabled(line, want);
  }
  // EXP_WAKE (active high, same ext1 ANY_HIGH mask): tamper, power-good and card
  // lines on the expanders wake the node, unless it is masked as stuck.
  if (p_.board.expander_wake_line != 0xFF) {
    sleep_->set_wake_enabled(p_.board.expander_wake_line, !exp_wake_masked_);
  }
  direct_wake_known_ = false;
  arm_direct_wake_lines();
}

void NodeRuntime::arm_direct_wake_lines() {
  if (!sleep_) return;
  std::uint32_t want = 0;
  std::uint32_t wired = 0;
  // Tamper loop: a wake source while closed (or not yet known). Once the opening
  // is reported the line stays high: masked until it is closed again (the
  // restore is then seen at the next step or heartbeat).
  if (const std::uint8_t line = p_.board.tamper_line; line < 32) {
    wired |= 1u << line;
    if (tamper_.state() != std::optional<bool>{true}) want |= 1u << line;
  }
  // USER: a wake source while released; masked while held.
  if (const std::uint8_t line = p_.board.user_button_line; line < 32) {
    wired |= 1u << line;
    if (!p_.gpio.read(line)) want |= 1u << line;
  }
  if (!wired || (direct_wake_known_ && want == direct_wake_armed_)) return;
  for (std::uint8_t line = 0; line < 32; ++line) {
    const auto bit = 1u << line;
    if ((wired & bit) && (!direct_wake_known_ || ((want ^ direct_wake_armed_) & bit))) {
      sleep_->set_wake_enabled(line, (want & bit) != 0);
    }
  }
  direct_wake_armed_ = want;
  direct_wake_known_ = true;
}

// --- expanders, tamper and indicators ---------------------------------------------------

void NodeRuntime::refresh_external_power() {
  const PowerSource s = p_.power.source();
  external_power_ = s == PowerSource::kUsb || s == PowerSource::kSolarBattery;
}

std::optional<bool> NodeRuntime::tamper_raw() {
  if (p_.board.tamper_line != 0xFF) return p_.gpio.read(p_.board.tamper_line);  // high = open
  return p_.power.tamper_open();
}

void NodeRuntime::sample_tamper(TimeMs now) {
  const bool known = tamper_.state().has_value();
  const bool changed = tamper_.sample(tamper_raw(), now);
  // While the loop is open the door input is re-read at every sample: a door
  // closed while the lid stays open (or the reverse) is a change of cause.
  if (changed || tamper_.state() == std::optional<bool>{true}) update_tamper_causes(now, !known);
}

// The loop is SW4 (lid) in series with SW5 (battery door); TAMPER_DOOR (EXP P11)
// is high only when the door is open, and while it is, the lid can't be seen.
// Boards without the door input report every opening as the enclosure, as before.
void NodeRuntime::update_tamper_causes(TimeMs now, bool first) {
  const auto loop = tamper_.state();
  // A loop change still debouncing: the door input may already show the new
  // state (the door shut a few ms before the loop settles) and would read as
  // the lid opening. Decide once the loop has settled.
  if (!loop || tamper_.pending()) return;
  bool lid = lid_reported_;
  bool door = door_reported_;
  const bool door_wired = p_.board.battery_door.wired();
  if (!*loop) {
    lid = false;
    door = false;
  } else {
    const std::optional<bool> d =
        door_wired ? p_.power.battery_door_open() : std::optional<bool>{false};
    if (d == std::optional<bool>{true}) {
      door = true;  // the lid keeps its last known state
    } else if (d == std::optional<bool>{false}) {
      door = false;
      lid = true;
    } else {
      lid = true;  // can't tell which: the enclosure (fails toward the alarm)
    }
  }
  // Cold boot: state reports (b = 1), so the hub also learns that a lid it saw
  // open was closed while the node was off. Afterwards, transitions (b = 0).
  const std::uint32_t b = first ? 1u : 0u;
  if (first || lid != lid_reported_) {
    if (lid_reported_ && !lid) mount_rebaseline_at_ = now + kMountSettleMs;  // closed after service
    lid_reported_ = lid;
    emit(kNoSlot, EventKind::kTamper, lid ? Priority::kP0Critical : Priority::kP1State, lid,
         static_cast<std::int32_t>(TamperCause::kEnclosure), b);
  }
  if (door_wired && (first || door != door_reported_)) {
    door_reported_ = door;
    emit(kNoSlot, EventKind::kTamper, door ? Priority::kP0Critical : Priority::kP1State, door,
         static_cast<std::int32_t>(TamperCause::kBatteryDoor), b);
  }
}

void NodeRuntime::check_magnet(std::uint8_t i) {
  if (i >= kSlots || !slots_[i].enabled) return;
  const auto* tmag = std::get_if<Tmag5273ContactDriver>(&slots_[i].driver);
  if (!tmag || tmag->field_seq() == magnet_seq_seen_) return;
  magnet_seq_seen_ = tmag->field_seq();
  if (tmag->open()) {
    // An open door can't be faked closed: the baseline goes, and a defeat ends.
    if (magnet_.defeated()) {
      emit(i, EventKind::kTamper, Priority::kP1State, false,
           static_cast<std::int32_t>(TamperCause::kMagnetDefeat), 0);
    }
    magnet_.clear_baseline();
    magnet_.restore(std::nullopt, false);
    magnet_was_open_ = true;
    return;
  }
  if (magnet_was_open_ || !magnet_.baseline()) {
    magnet_.set_baseline(tmag->field());  // the door just closed: its own magnet
    magnet_was_open_ = false;
    return;
  }
  if (const auto change = magnet_.check(tmag->field())) {
    emit(i, EventKind::kTamper, *change ? Priority::kP0Critical : Priority::kP1State, *change,
         static_cast<std::int32_t>(TamperCause::kMagnetDefeat), 0);
  }
}

void NodeRuntime::check_mount(std::uint8_t i, TimeMs now) {
  if (i >= kSlots || !slots_[i].enabled) return;
  auto* imu = std::get_if<Lis2dw12Driver>(&slots_[i].driver);
  if (!imu) return;
  // Re-baseline only with the enclosure shut: someone working on it moves it.
  const bool rebaseline =
      now >= mount_rebaseline_at_ && tamper_.state() == std::optional<bool>{false};
  if (!rebaseline && !mount_.baseline()) return;
  SlotIo io(p_, i);
  Vec3 g;
  const bool ok = imu->read_gravity(io, g);
  io.deselect();
  if (!ok) return;
  if (rebaseline) {
    if (!MountPose::plausible(g)) return;  // try again at the next heartbeat
    const bool was_moved = mount_.moved();
    mount_.set_baseline(g);
    mount_rebaseline_at_ = kNever;
    const std::array<std::int32_t, 3> raw{g.x, g.y, g.z};
    save(p_.store, StoreKey::kMountBaseline, raw);
    if (was_moved) {
      emit(i, EventKind::kTamper, Priority::kP1State, false,
           static_cast<std::int32_t>(TamperCause::kMountMoved), 0);
    }
    return;
  }
  if (const auto change = mount_.check(g)) {
    emit(i, EventKind::kTamper, *change ? Priority::kP0Critical : Priority::kP1State, *change,
         static_cast<std::int32_t>(TamperCause::kMountMoved), 0);
  }
}

void NodeRuntime::apply_tx_power() {
  const bool cap = p_.board.primary_battery && !external_power_ && battery_mv_ != 0 &&
                   battery_mv_ < p_.board.battery_low_mv;
  tx_capped_ = cap && p_.board.tx_power_low_battery_dbm != 0x7F;
  const std::int8_t want = tx_capped_ ? p_.board.tx_power_low_battery_dbm : p_.board.tx_power_dbm;
  if (want == 0x7F || want == tx_applied_) return;
  if (p_.link.set_tx_power_dbm(want)) tx_applied_ = want;
}

PowerProfile NodeRuntime::power_profile() const {
  const std::uint8_t v = config_.power_profile;
  if (v <= static_cast<std::uint8_t>(PowerProfile::kAlwaysOn)) return static_cast<PowerProfile>(v);
  return p_.board.power_profile;
}

SleepPlan NodeRuntime::sleep_plan(bool console_active) const {
  SleepInputs in;
  in.profile = power_profile();
  in.external_power = external_power_;
  in.sends_pending = queue_.size() > 0;
  in.console_active = console_active;
  in.busy = downlink_more_ || ota_.record().state == OtaState::kDownloading || tamper_.pending() ||
            button_.pending() || user_.pending() || lamp_test_active() ||
            mount_rebaseline_at_ != kNever || mount_check_at_ != kNever || service_mode();
  in.now_ms = p_.clock.now_ms();
  in.next_wake_ms = next_wake_ms();
  return SleepPolicy::plan(in);
}

// EXP_WAKE high means some expander saw an input change (TAMPER_N, CHG_PG_N,
// PMUX_ST, PRESENT_N, ALERT_N, SW_FAULT_N). Reading every port releases INT.
// A line still high after that is held by a fault or a toggling input: after
// kWakeMaskMs it is masked as a wake source and polled every kMaskedPollMs,
// exactly like a stuck slot WAKE line.
void NodeRuntime::service_expanders(TimeMs now) {
  const std::uint8_t line = p_.board.expander_wake_line;
  if (line == 0xFF) return;
  if (p_.gpio.read(line) && (!exp_wake_masked_ || now >= next_exp_poll_)) {
    next_exp_poll_ = now + kMaskedPollMs;
    p_.power.service_interrupt();
    refresh_external_power();
    sample_tamper(now);
  }
  if (!p_.gpio.read(line)) {
    exp_wake_high_ = false;
    if (exp_wake_masked_) {
      exp_wake_masked_ = false;
      if (sleep_) sleep_->set_wake_enabled(line, true);
    }
    return;
  }
  if (!exp_wake_high_) {
    exp_wake_high_ = true;
    exp_wake_high_since_ = now;
  }
  if (!exp_wake_masked_ && now - exp_wake_high_since_ >= kWakeMaskMs) {
    exp_wake_masked_ = true;
    if (sleep_) sleep_->set_wake_enabled(line, false);
    next_exp_poll_ = now + kMaskedPollMs;
  }
}

// BOOT is a strap pin: its level matters only while the chip comes out of
// reset (download mode is entered by the ROM, before any of this runs). After
// boot it is just a pulled-up input, read here and never driven.
void NodeRuntime::poll_service_button(TimeMs now) {
  const std::uint8_t line = p_.board.service_button_line;
  if (line == 0xFF) return;
  if (!button_.sample(!p_.gpio.read(line), now)) return;
  if (service_mode()) {
    stop_service_mode();
  } else {
    start_service_mode();  // refused with the lid closed on LampGate::kLidOpen boards
  }
}

// USER (TL-SN-002): short press = test report (a heartbeat now) plus a 10 s lamp
// test; long press = a commissioning/pairing request to the hub. Both are P1
// kUserButton events: the hub logs them (diagnostics) and never acts on its own.
void NodeRuntime::poll_user_button(TimeMs now) {
  const std::uint8_t line = p_.board.user_button_line;
  if (line == 0xFF) return;
  switch (user_.sample(p_.gpio.read(line), now)) {
    case UserButton::Press::kNone:
      return;
    case UserButton::Press::kShort: {
      if (last_user_report_ms_ != kNever && now - last_user_report_ms_ < kUserCooldownMs) return;
      last_user_report_ms_ = now;
      emit(kNoSlot, EventKind::kUserButton, Priority::kP1State, false,
           static_cast<std::int32_t>(UserButtonAction::kTestReport), 0);
      const std::uint32_t mv = take_battery_mv();
      emit(kNoSlot, EventKind::kHeartbeat, Priority::kP2Health, false,
           static_cast<std::int32_t>(mv), static_cast<std::uint32_t>(mode_));
      start_lamp_test(now);  // the event and heartbeat always; the lamps only if allowed
      return;
    }
    case UserButton::Press::kLong:
      if (last_user_commission_ms_ != kNever && now - last_user_commission_ms_ < kUserCooldownMs) {
        return;
      }
      last_user_commission_ms_ = now;
      emit(kNoSlot, EventKind::kUserButton, Priority::kP1State, true,
           static_cast<std::int32_t>(UserButtonAction::kCommissionRequest), 0);
      start_lamp_test(now);
      return;
  }
}

// TL-SN-002 (LampGate::kLidOpen): the USER lamp test runs only with the lid open,
// so a press on a closed node (or a stuck button) never makes it visible from outside.
void NodeRuntime::start_lamp_test(TimeMs now) {
  if (p_.board.lamp_gate == LampGate::kLidOpen && tamper_.state() != std::optional<bool>{true}) {
    return;
  }
  lamp_test_started_ = now;
  lamp_test_until_ = now + kLampTestMs;
  update_indicators(now);
}

bool NodeRuntime::start_service_mode() {
  const TimeMs now = p_.clock.now_ms();
  if (p_.board.lamp_gate == LampGate::kLidOpen && tamper_.state() != std::optional<bool>{true}) {
    return service_mode();  // the BOOT button is inside: service mode needs the lid open
  }
  slot_leds_.start_service(now);
  update_indicators(now);  // at once, not at the next step
  return true;
}

bool NodeRuntime::lamps_unattended_ok() const {
  if (p_.board.lamp_gate == LampGate::kLidOpen) return tamper_.state() == std::optional<bool>{true};
  return external_power_;
}

bool NodeRuntime::console_locked() const {
  return p_.board.console_lock == ConsoleLock::kLidClosed &&
         tamper_.state() != std::optional<bool>{true};
}

void NodeRuntime::note_console_refusal() {
  ++console_refused_total_;
  ++console_unreported_;
  report_console_refusals(p_.clock.now_ms());
}

// One kConsoleLocked diagnostic per kConsoleReportMs at most: the first refusal at
// once, later ones counted and sent when the window ends (step()). P1, never P0:
// it is evidence for the hub's health view, not an alarm (the alarm is the lid).
void NodeRuntime::report_console_refusals(TimeMs now) {
  if (console_unreported_ == 0) return;
  if (console_reported_at_ != kNever && now - console_reported_at_ < kConsoleReportMs) return;
  emit(kNoSlot, EventKind::kConsoleLocked, Priority::kP1State, false,
       static_cast<std::int32_t>(console_unreported_), console_refused_total_);
  console_unreported_ = 0;
  console_reported_at_ = now;
}

void NodeRuntime::stop_service_mode() {
  slot_leds_.stop_service();
  update_indicators(p_.clock.now_ms());
}

StatusInputs NodeRuntime::status_inputs(TimeMs now) const {
  StatusInputs in;
  in.since_boot_ms = now - boot_ms_;
  in.cold_boot = !wake_.deep_sleep();
  in.hub_heard = hub_heard_;
  in.link_failing = outage_;
  const OtaState ota = ota_.record().state;
  in.ota_active = ota == OtaState::kDownloading || ota == OtaState::kPendingVerify;
  in.low_battery = mode_ != PowerMode::kNormal;
  in.card_error = quarantined_ != 0;
  for (const auto& st : slots_) {
    if (st.fault != ModuleFault::kNone && st.fault != ModuleFault::kDisabledForPower) {
      in.card_error = true;
    }
  }
  in.fault = !board_ok_ || !epoch_persisted_;
  in.external_power = external_power_;
  return in;
}

std::array<SlotLedMode, kSlots> NodeRuntime::slot_led_modes() const {
  std::array<SlotLedMode, kSlots> modes{};
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    const SlotState& st = slots_[i];
    if ((quarantined_ & (1u << i)) ||
        (st.fault != ModuleFault::kNone && st.fault != ModuleFault::kDisabledForPower)) {
      modes[i] = SlotLedMode::kSlowBlink;
    } else if (st.enabled) {
      modes[i] = SlotLedMode::kSolid;
    }
  }
  return modes;
}

void NodeRuntime::update_indicators(TimeMs now) {
  led_due_ms_ = kNever;
  // LampGate::kLidOpen: closing the lid ends a running USER lamp test at once.
  if (p_.board.lamp_gate == LampGate::kLidOpen && now < lamp_test_until_ &&
      tamper_.state() != std::optional<bool>{true}) {
    lamp_test_until_ = now;
  }
  if (const std::uint8_t line = p_.board.status_led_line; line != 0xFF) {
    const StatusCode c = select_status(status_inputs(now));
    if (c != status_) {
      status_ = c;
      status_since_ = now;
    }
    // LampGate::kLidOpen (TL-SN-002): STAT too stays dark with the lid closed
    // unless service mode, so nothing on the board is visible from outside.
    const bool stat_allowed = p_.board.lamp_gate != LampGate::kLidOpen || lamps_unattended_ok() ||
                              slot_leds_.service_active(now);
    const bool on = stat_allowed && status_led_on(status_, now - status_since_);
    if (!status_led_known_ || on != status_led_level_) {
      status_led_known_ = p_.gpio.write(line, !on);  // active low
      status_led_level_ = on;
    }
    if (stat_allowed) {
      const TimeMs edge = status_next_edge(status_, now - status_since_);
      if (edge != kNever) led_due_ms_ = status_since_ + edge;
      if (!lamps_unattended_ok() && slot_leds_.service_active(now) &&
          slot_leds_.service_until() < led_due_ms_) {
        led_due_ms_ = slot_leds_.service_until();
      }
    }
  }
  update_lamps(now);
  bool wired = false;
  for (const auto& led : p_.board.slot_led_n) wired |= led.wired();
  if (!wired) return;
  const auto modes = slot_led_modes();
  const bool unattended = lamps_unattended_ok();
  if (const TimeMs t = slot_leds_.next_change(modes, now, unattended); t < led_due_ms_) {
    led_due_ms_ = t;
  }
  std::uint8_t want = 0;
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    if (slot_leds_.led_on(i, modes[i], now, unattended)) {
      want = static_cast<std::uint8_t>(want | (1u << i));
    }
  }
  if (now < lamp_test_until_) want = lamp_test_on(lamp_test_started_, now) ? 0x0F : 0x00;
  if (slot_leds_known_ && want == slot_led_shadow_) return;
  bool ok = true;
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    const auto bit = static_cast<std::uint8_t>(1u << i);
    if (!p_.board.slot_led_n[i].wired()) continue;
    if (!slot_leds_known_ || ((want ^ slot_led_shadow_) & bit)) {
      ok &= p_.power.set_slot_led(i, (want & bit) != 0);
    }
  }
  slot_led_shadow_ = want;
  slot_leds_known_ = ok;
}

// TL-SN-002 board lamps (Lamp): same rule as the slot LEDs (BoardSupport::lamp_gate:
// on TL-SN-002 dark unless the lid is open or service mode), except the USER lamp
// test, which blinks them all (and itself only starts with the lid open there).
void NodeRuntime::update_lamps(TimeMs now) {
  bool wired = false;
  for (const auto& l : p_.board.lamp_n) wired |= l.wired();
  if (!wired) return;
  auto consider = [&](TimeMs t) {
    if (t > now && t < led_due_ms_) led_due_ms_ = t;
  };
  std::uint8_t want = 0;
  if (now < lamp_test_until_) {
    want = lamp_test_on(lamp_test_started_, now) ? 0x0F : 0x00;
    const TimeMs k = (now - lamp_test_started_) / kLampTestHalfMs + 1;
    consider(std::min<TimeMs>(lamp_test_started_ + k * kLampTestHalfMs, lamp_test_until_));
  } else {
    const StatusInputs st = status_inputs(now);
    LampInputs in;
    const bool unattended = lamps_unattended_ok();
    in.lit_allowed = slot_leds_.lit_allowed(now, unattended);
    in.joined = hub_heard_ && !outage_;
    in.tamper_open = tamper_.state();
    in.battery_low = battery_low();
    in.fault = st.fault || st.card_error || now < fault_lamp_until_;
    want = lamp_levels(in, now);
    if (!unattended && slot_leds_.service_active(now)) consider(slot_leds_.service_until());
    if (in.lit_allowed) {
      if (!in.joined) {
        const TimeMs t = now % SlotLedPolicy::kSlowBlinkPeriodMs;
        consider(now - t +
                 (t < SlotLedPolicy::kSlowBlinkOnMs ? SlotLedPolicy::kSlowBlinkOnMs
                                                    : SlotLedPolicy::kSlowBlinkPeriodMs));
      }
      if (now < fault_lamp_until_) consider(fault_lamp_until_);
    }
  }
  if (lamps_known_ && want == lamp_shadow_) return;
  bool ok = true;
  for (std::size_t i = 0; i < kLampCount; ++i) {
    const auto bit = static_cast<std::uint8_t>(1u << i);
    if (!p_.board.lamp_n[i].wired()) continue;
    if (!lamps_known_ || ((want ^ lamp_shadow_) & bit)) {
      ok &= p_.power.set_lamp(static_cast<Lamp>(i), (want & bit) != 0);
    }
  }
  lamp_shadow_ = want;
  lamps_known_ = ok;
}

PowerMode NodeRuntime::compute_mode(std::uint32_t mv) {
  if (p_.power.source() == PowerSource::kUsb) return PowerMode::kNormal;
  // Hysteresis: staying in a lower mode needs only to be under threshold + margin.
  const std::uint32_t h = p_.board.battery_hysteresis_mv;
  const std::uint32_t crit =
      p_.board.battery_critical_mv + (mode_ == PowerMode::kCritical ? h : 0u);
  const std::uint32_t low = p_.board.battery_low_mv + (mode_ != PowerMode::kNormal ? h : 0u);
  if (mv < crit) return PowerMode::kCritical;
  if (mv < low) return PowerMode::kLowPower;
  return PowerMode::kNormal;
}

std::uint32_t NodeRuntime::take_battery_mv() {
  const std::uint32_t now_mv = p_.power.battery_mv();
  if (!p_.board.primary_battery) {
    battery_mv_ = now_mv;
    return now_mv;
  }
  // Li-FeS2 rests near 1.8 V/cell until it is almost empty; only the voltage
  // under load shows the end of life, so the lowest reading since the last
  // heartbeat (taken right after radio sends) is the battery level.
  battery_mv_ = std::min(loaded_mv_, now_mv);
  loaded_mv_ = ~std::uint32_t{0};
  return battery_mv_;
}

void NodeRuntime::sample_battery_under_load() {
  if (p_.board.primary_battery) loaded_mv_ = std::min(loaded_mv_, p_.power.battery_mv());
}

void NodeRuntime::update_power_mode(std::uint32_t mv) {
  const PowerMode m = compute_mode(mv);
  if (m == mode_) return;
  mode_ = m;
  emit(kNoSlot, EventKind::kPowerMode, Priority::kP1State, false, static_cast<std::int32_t>(m), 0);
  admit_power();
}

void NodeRuntime::handle(const Downlink& d) {
  if (const auto* ack = std::get_if<Ack>(&d)) {
    queue_.ack(ack->boot_epoch, ack->seq);
    note_ack(ack->boot_epoch, ack->seq, p_.clock.now_ms());  // NVS later (flush_acks)
    // Health check for a trial image: it reached the hub end to end, and the
    // board took its safe state at boot. Only then is it marked valid (here and
    // in the bootloader); otherwise the next reboot rolls it back.
    if (ota_.record().state == OtaState::kPendingVerify && board_ok_ && ota_.confirm()) {
      emit_ota_status(Priority::kP1State);
    }
  } else if (const auto* cfg = std::get_if<ConfigUpdate>(&d)) {
    // Stale, duplicate, or the maximum revision (it would lock every later
    // config out for good: red team RT-14).
    if (cfg->revision <= config_.revision ||
        cfg->revision == std::numeric_limits<std::uint32_t>::max()) {
      return;
    }
    RuntimeConfig next = config_;
    next.revision = cfg->revision;
    // Fields outside their bounds are ignored, the rest applies.
    if (cfg->heartbeat_interval_ms >= kMinIntervalMs &&
        cfg->heartbeat_interval_ms <= kMaxHeartbeatIntervalMs) {
      next.heartbeat_interval_ms = cfg->heartbeat_interval_ms;
    }
    if (cfg->environment_interval_ms >= kMinIntervalMs &&
        cfg->environment_interval_ms <= kMaxEnvironmentIntervalMs) {
      next.environment_interval_ms = cfg->environment_interval_ms;
    }
    if (cfg->impact_threshold_mg >= 32 && cfg->impact_threshold_mg <= 1968) {
      next.impact_threshold_mg = cfg->impact_threshold_mg;
    }
    if (cfg->power_profile <= static_cast<std::uint8_t>(PowerProfile::kAlwaysOn)) {
      next.power_profile = cfg->power_profile;
    }
    config_ = next;
    save(p_.store, StoreKey::kConfig, config_);
    for (std::uint8_t i = 0; i < kSlots; ++i) {
      if (auto* imu = std::get_if<Lis2dw12Driver>(&slots_[i].driver)) {
        SlotIo io(p_, i);
        imu->set_threshold(io, config_.impact_threshold_mg);
        io.deselect();
      }
    }
    emit(kNoSlot, EventKind::kConfigApplied, Priority::kP1State, false,
         static_cast<std::int32_t>(config_.revision), 0);
  } else if (const auto* offer = std::get_if<OtaOffer>(&d)) {
    // Primary cells: the worst loaded level known (last heartbeat's, any sag
    // since, and now), never just the resting voltage, which hides an empty pair.
    const std::uint32_t mv = p_.board.primary_battery
                                 ? std::min({battery_mv_, loaded_mv_, p_.power.battery_mv()})
                                 : p_.power.battery_mv();
    const bool power_ok =
        p_.power.source() == PowerSource::kUsb || mv >= p_.board.battery_ota_min_mv;
    ota_.offer(*offer, power_ok);
    emit_ota_status(Priority::kP2Health);
  } else if (const auto* chunk = std::get_if<OtaChunk>(&d)) {
    const OtaState before = ota_.record().state;
    if (ota_.chunk(*chunk)) ota_progress_ = true;
    if (ota_.record().state != before) emit_ota_status(Priority::kP2Health);
    if (ota_.reboot_wanted()) {
      flush_acks();
      reboot_requested_ = true;
    }
  } else if (std::holds_alternative<RebootRequest>(d)) {
    flush_acks();
    reboot_requested_ = true;
  }
}

// Heartbeat checks: card still there, sensor still armed, empty slot filled.
void NodeRuntime::supervise_slots(TimeMs now) {
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    if (!p_.board.slots[i].present || (quarantined_ & (1u << i))) continue;
    SlotState& st = slots_[i];
    if (fixed(i)) {
      // Built-in (ADR 0019): never "removed" (kCardRemoved can't happen) and never
      // hot-inserted. A failed or quarantined one is retried with backoff.
      if (!st.enabled) {
        if (st.fault != ModuleFault::kNone && st.fault != ModuleFault::kDisabledForPower &&
            now >= fixed_retry_at_[i]) {
          // The failure that put it here was counted then: 1 h, 2 h, 4 h ... 24 h.
          fixed_retry_at_[i] = now + fixed_backoff_ms(i);
          fixed_quarantined_ = static_cast<std::uint8_t>(fixed_quarantined_ & ~(1u << i));
          unpower_slot(i);  // the PIR: its rail goes off, rescan powers it again
          rescan(i);
        }
        continue;
      }
      if (std::holds_alternative<std::monostate>(st.driver)) continue;
      SlotIo io(p_, i);
      std::visit(
          [&](auto& drv) {
            if constexpr (requires { drv.supervise(io, *this, now); }) {
              drv.supervise(io, *this, now);
            }
          },
          st.driver);
      io.deselect();
      continue;
    }
    if (!st.enabled) {
      // Hot insertion (or the card put back): discover it.
      if (st.code != ModuleCode::kNone && st.fault != ModuleFault::kCardRemoved) continue;
      const std::optional<bool> seated = p_.power.slot_present(i);
      bool found = seated.value_or(false);
      if (!seated) {  // the board can't tell: its AON is unswitched, ask the ID EEPROM
        SlotIo io(p_, i);
        found = read_module_id(io, p_.board.id_eeprom_address).status == IdRead::Status::kOk;
        io.deselect();
      }
      if (found) rescan(i);
      continue;
    }
    if (std::holds_alternative<std::monostate>(st.driver)) continue;
    bool gone = p_.power.slot_present(i) == std::optional<bool>{false};
    IdRead id;
    if (!gone) {
      for (int attempt = 0; attempt < 2; ++attempt) {  // one glitch is not a removal
        SlotIo io(p_, i);
        id = read_module_id(io, p_.board.id_eeprom_address);
        io.deselect();
        if (id.status != IdRead::Status::kAbsent) break;
      }
      gone = id.status == IdRead::Status::kAbsent ||
             (id.status == IdRead::Status::kOk && id.code != st.code);
    }
    if (gone) {
      emit(i, EventKind::kModuleFault, Priority::kP2Health, false,
           static_cast<std::int32_t>(ModuleFault::kCardRemoved),
           static_cast<std::uint32_t>(st.code));
      st.enabled = false;
      st.fault = ModuleFault::kCardRemoved;
      st.driver = std::monostate{};
      unpower_slot(i);
      arm_wake_lines();
      if (id.status == IdRead::Status::kOk) rescan(i);  // a different card: discover it
      continue;
    }
    SlotIo io(p_, i);
    std::visit(
        [&](auto& drv) {
          if constexpr (requires { drv.supervise(io, *this, now); }) drv.supervise(io, *this, now);
        },
        st.driver);
    io.deselect();
  }
}

// A wake line held high would re-wake the node from deep sleep forever: mask
// it as a wake source after kWakeMaskMs and poll it slowly until it falls.
void NodeRuntime::supervise_wake_lines(TimeMs now) {
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    const std::uint8_t line = p_.board.slots[i].wake_line;
    const auto bit = static_cast<std::uint8_t>(1u << i);
    if (line == 0xFF || !slots_[i].enabled) {
      wake_high_since_[i] = kNever;
      continue;
    }
    if (!p_.gpio.read(line)) {
      wake_high_since_[i] = kNever;
      wake_stuck_reported_ = static_cast<std::uint8_t>(wake_stuck_reported_ & ~bit);
      if (wake_masked_ & bit) {
        wake_masked_ = static_cast<std::uint8_t>(wake_masked_ & ~bit);
        if (sleep_) sleep_->set_wake_enabled(line, true);
      }
      continue;
    }
    if (wake_high_since_[i] == kNever) wake_high_since_[i] = now;
    const TimeMs high_ms = now - wake_high_since_[i];
    if (!(wake_masked_ & bit) && high_ms >= kWakeMaskMs) {
      wake_masked_ = static_cast<std::uint8_t>(wake_masked_ | bit);
      if (sleep_) sleep_->set_wake_enabled(line, false);
      next_masked_poll_[i] = now;
    }
    // A contact held open is a state, not a fault; the LIS2DW12 reports its own.
    const bool self_explained = std::holds_alternative<ContactDriver>(slots_[i].driver) ||
                                std::holds_alternative<Lis2dw12Driver>(slots_[i].driver);
    if (!self_explained && !(wake_stuck_reported_ & bit) && high_ms >= kWakeStuckFaultMs) {
      wake_stuck_reported_ = static_cast<std::uint8_t>(wake_stuck_reported_ | bit);
      emit(i, EventKind::kModuleFault, Priority::kP2Health, false,
           static_cast<std::int32_t>(ModuleFault::kStuckInterrupt),
           static_cast<std::uint32_t>(slots_[i].code));
    }
  }
}

int NodeRuntime::send_due(TimeMs now) {
  // Nothing leaves before the epoch is persisted: the hub would drop a reused
  // (epoch, seq) as a duplicate. Events wait in the queue meanwhile.
  if (!epoch_persisted_) return 0;
  if (now > last_refill_ms_) {
    const TimeMs add = (now - last_refill_ms_) / kSendRefillMs;
    if (add > 0) {
      tokens_ = static_cast<std::uint32_t>(std::min<TimeMs>(kSendBurst, tokens_ + add));
      last_refill_ms_ += add * kSendRefillMs;
    }
  }
  if (!outage_ && unanswered_ >= kLinkFailSends && now - last_reply_ms_ >= kLinkFailMs) {
    outage_ = true;  // the hub is gone or the air is jammed: ration the radio
    probe_backoff_ms_ = kProbeMinMs;
    next_probe_ms_ = now + probe_backoff_ms_;
  }
  int budget = kSendsPerStep;
  if (outage_) {
    const bool p0_probe = p0_waiting_ && now - last_probe_ms_ >= kP0ProbeMinMs;
    if (now < next_probe_ms_ && !p0_probe) return 0;
    budget = 1;  // one probe: the most important event, whatever its own backoff
  }
  int sent = 0;
  for (int n = 0; n < budget; ++n) {
    auto* s = queue_.next_due(outage_ ? kNever : now);
    if (!s) break;
    if (s->event.priority != Priority::kP0Critical) {
      if (tokens_ == 0) break;
      --tokens_;
    }
    p_.link.send(s->event);
    queue_.mark_sent(*s, now);  // backoff whether or not the radio took it
    ++unanswered_;
    ++sent;
  }
  if (outage_ && sent > 0) {
    last_probe_ms_ = now;
    p0_waiting_ = false;
    if (now >= next_probe_ms_) {
      next_probe_ms_ = now + probe_backoff_ms_;
      probe_backoff_ms_ = std::min(probe_backoff_ms_ * 2, kProbeMaxMs);
    }
  }
  return sent;
}

TimeMs NodeRuntime::send_gate_ms(TimeMs now) const {
  if (!outage_) return tokens_ == 0 ? last_refill_ms_ + kSendRefillMs : now;
  TimeMs gate = next_probe_ms_;
  if (p0_waiting_ && last_probe_ms_ + kP0ProbeMinMs < gate) gate = last_probe_ms_ + kP0ProbeMinMs;
  return gate;
}

void NodeRuntime::step() {
  feed_watchdog();
  const TimeMs now = p_.clock.now_ms();
  if (!epoch_persisted_ && now >= next_epoch_retry_) {
    epoch_persisted_ = persist_boot_epoch();
    next_epoch_retry_ = now + kEpochRetryMs;
  }
  // Bounded: a flood of downlinks can't hold the step (and the watchdog) forever.
  bool replied = false;
  int handled = 0;
  for (; handled < kMaxDownlinksPerStep; ++handled) {
    auto d = p_.link.receive();
    if (!d) break;
    handle(*d);
    replied = true;
    feed_watchdog();
  }
  downlink_more_ = handled == kMaxDownlinksPerStep;
  if (replied) {
    hub_heard_ = true;
    unanswered_ = 0;
    last_reply_ms_ = now;
    outage_ = false;
    probe_backoff_ms_ = kProbeMinMs;
  }
  // One progress report per batch of chunks: the hub streams the next ones
  // in its reply (device-initiated exchange, sleepy-node friendly).
  if (ota_progress_ && ota_.record().state == OtaState::kDownloading) {
    emit_ota_status(Priority::kP2Health);
  }
  ota_progress_ = false;

  supervise_wake_lines(now);
  service_expanders(now);
  if (p_.board.tamper_line != 0xFF) {
    sample_tamper(now);  // a direct GPIO: cheap to read every step
    tamper_poll_at_ = tamper_.state() == std::optional<bool>{true} ? now + kMaskedPollMs : kNever;
  } else if (tamper_.pending() && now >= tamper_.due_ms()) {
    sample_tamper(now);
  }
  if (p_.board.usb_present_line != 0xFF) {
    // USB_PRESENT is a plain GPIO (no expander INT): read it every step.
    const bool before = external_power_;
    refresh_external_power();
    if (external_power_ != before) {
      update_power_mode(battery_mv_);
      apply_tx_power();
    }
  }
  poll_service_button(now);
  poll_user_button(now);
  const std::uint32_t env_interval = static_cast<std::uint32_t>(
      std::min<TimeMs>(static_cast<TimeMs>(config_.environment_interval_ms) *
                           (mode_ == PowerMode::kNormal ? 1u : 4u),
                       std::numeric_limits<std::uint32_t>::max()));
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    SlotState& st = slots_[i];
    if (!st.enabled) continue;
    if (wake_masked_ & (1u << i)) {  // stuck high: poll it slowly, not every step
      if (now < next_masked_poll_[i]) continue;
      next_masked_poll_[i] = now + kMaskedPollMs;
    }
    SlotIo io(p_, i);
    std::visit(
        [&](auto& drv) {
          using T = std::decay_t<decltype(drv)>;
          if constexpr (std::is_same_v<T, Sht40Driver>) {
            if (mode_ != PowerMode::kCritical) drv.poll(io, *this, now, env_interval);
          } else if constexpr (!std::is_same_v<T, std::monostate>) {
            drv.poll(io, *this, now);
          }
        },
        st.driver);
    io.deselect();
    check_magnet(i);
  }
  if (now >= mount_check_at_) {
    mount_check_at_ = kNever;
    for (std::uint8_t i = 0; i < kSlots; ++i) check_mount(i, now);
  }
  if (mount_rebaseline_at_ != kNever && now >= mount_rebaseline_at_) {
    for (std::uint8_t i = 0; i < kSlots; ++i) check_mount(i, now);
  }
  if (quarantine_pending_) {
    for (std::uint8_t i = 0; i < kSlots; ++i) {
      if (!(quarantine_pending_ & (1u << i))) continue;
      // A PIR whose warm-up timed out but that was still toggling (someone kept
      // moving, RT-08) ended its warm-up and reported the motion: it works, so it
      // is not disabled. Only one still stuck in warm-up is quarantined.
      const auto* pir = std::get_if<PirDriver>(&slots_[i].driver);
      if (quarantine_why_[i] == ModuleFault::kWarmupTimeout && pir && !pir->warming()) continue;
      quarantine_fixed(i, quarantine_why_[i], now);
    }
    quarantine_pending_ = 0;
  }

  if (const std::uint8_t tripped = p_.power.take_sw_faults()) {
    for (std::uint8_t i = 0; i < kSlots; ++i) {
      if (!(tripped & (1u << i))) continue;
      emit(i, EventKind::kModuleFault, Priority::kP2Health, false,
           static_cast<std::int32_t>(ModuleFault::kRailFault),
           static_cast<std::uint32_t>(slots_[i].code));
    }
  }

  if (now >= next_heartbeat_) {
    p_.power.service();
    refresh_external_power();
    sample_tamper(now);  // belt and braces: a missed INT can't hide an open lid past one heartbeat
    const std::uint32_t mv = take_battery_mv();
    update_power_mode(mv);
    apply_tx_power();
    supervise_slots(now);
    for (std::uint8_t i = 0; i < kSlots; ++i) {
      check_magnet(i);  // supervise() took a fresh field while the door is closed
      check_mount(i, now);
    }
    emit(kNoSlot, EventKind::kHeartbeat, Priority::kP2Health, false, static_cast<std::int32_t>(mv),
         static_cast<std::uint32_t>(mode_));
    const std::uint32_t factor = mode_ == PowerMode::kNormal ? 1u : 4u;
    next_heartbeat_ = now + static_cast<TimeMs>(config_.heartbeat_interval_ms) * factor;
  }

  if (quarantined_) retry_quarantined(now);
  report_console_refusals(now);

  // Non-critical telemetry has a shelf life: after an outage only fresh data goes out.
  queue_.remove_if([now](const DeviceEvent& e) {
    return e.priority >= Priority::kP2Health && e.at_ms + kTelemetryTtlMs <= now;
  });
  if (acks_dirty_ && now - acks_dirty_since_ >= kAckFlushMs) flush_acks();

  if (send_due(now) > 0) sample_battery_under_load();  // the cells right after a TX burst
  arm_direct_wake_lines();
  update_indicators(now);
  awake_ = false;
  save_retained();
}

TimeMs NodeRuntime::next_wake_ms() const {
  const TimeMs now = p_.clock.now_ms();
  if (downlink_more_) return now;
  TimeMs next = next_heartbeat_;
  if (!epoch_persisted_) {
    if (next_epoch_retry_ < next) next = next_epoch_retry_;
  } else if (auto due = queue_.earliest_due()) {
    const TimeMs gate = send_gate_ms(now);
    const TimeMs t = outage_ ? gate : std::max(*due, gate);
    if (t < next) next = t;
  }
  if (acks_dirty_ && acks_dirty_since_ + kAckFlushMs < next) next = acks_dirty_since_ + kAckFlushMs;
  if (quarantined_) {
    for (std::uint8_t i = 0; i < kSlots; ++i) {
      if ((quarantined_ & (1u << i)) && quarantine_retry_at_[i] < next) {
        next = quarantine_retry_at_[i];
      }
    }
  }
  if (tamper_.pending() && tamper_.due_ms() < next) next = tamper_.due_ms();
  if (console_unreported_ && console_reported_at_ + kConsoleReportMs < next) {
    next = console_reported_at_ + kConsoleReportMs;
  }
  if (button_.pending() && button_.due_ms() < next) next = button_.due_ms();
  if (user_.pending() && user_.due_ms() < next) next = user_.due_ms();
  if (tamper_poll_at_ < next) next = tamper_poll_at_;
  if (mount_rebaseline_at_ < next) next = mount_rebaseline_at_;
  if (mount_check_at_ < next) next = mount_check_at_;
  if (p_.board.expander_wake_line != 0xFF && exp_wake_high_) {
    const TimeMs t = exp_wake_masked_ ? next_exp_poll_ : exp_wake_high_since_ + kWakeMaskMs;
    if (t < next) next = t;
  }
  // LED edges, computed when the LEDs were last updated (not from `now`, which
  // would always put the next edge in the future and never step for it).
  if (led_due_ms_ < next) next = led_due_ms_;
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    const SlotState& st = slots_[i];
    if (!st.enabled) continue;
    if (p_.board.slots[i].wake_line != 0xFF && wake_high_since_[i] != kNever &&
        !(wake_masked_ & (1u << i)) && wake_high_since_[i] + kWakeMaskMs < next) {
      next = wake_high_since_[i] + kWakeMaskMs;  // time to mask it
    }
    if (wake_masked_ & (1u << i)) {
      if (next_masked_poll_[i] < next) next = next_masked_poll_[i];
      continue;
    }
    const TimeMs due = std::visit(
        [now](const auto& drv) -> TimeMs {
          if constexpr (std::is_same_v<std::decay_t<decltype(drv)>, std::monostate>) {
            return kNever;
          } else {
            return drv.due_ms(now);
          }
        },
        st.driver);
    if (due < next) next = due;
  }
  return next < now ? now : next;
}

}  // namespace talos::fw
