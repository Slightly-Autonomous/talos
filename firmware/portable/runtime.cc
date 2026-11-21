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
constexpr std::uint32_t kRetainedMagic = 0x544C5331u;  // "TLS1"
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
  std::uint32_t crc;
};
static_assert(std::is_trivially_copyable_v<RetainedState>);

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
  if (kind == EventKind::kModuleFault && priority != Priority::kP0Critical &&
      is_supervision_fault(a, b) && slot_critical(slot)) {
    priority = Priority::kP0Critical;  // intrusion sensing is lost: a trouble alarm
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
  // L-03: a wake event from a slot is a short flash of its LED (when lit at all).
  if (slot < kSlots &&
      (kind == EventKind::kMotion || kind == EventKind::kContact || kind == EventKind::kImpact)) {
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
  if (warm && (rs.flags & 4u)) tamper_.restore((rs.flags & 8u) != 0);
  exp_wake_masked_ = warm && (rs.flags & 16u);
  exp_wake_high_ = false;
  next_exp_poll_ = t0;
  status_ = StatusCode::kOff;
  status_led_known_ = false;
  slot_led_shadow_ = 0;
  slot_leds_known_ = !resumed_cards_;  // begin() wrote every slot LED dark
  mode_ = compute_mode();
  Resume r;
  if (resumed_cards_) {
    r.active = true;
    r.code = rs.code;
    r.state = rs.state;
  }
  discover(r);
  admit_power();
  refresh_external_power();
  // EXP_WAKE first: on a wake from it this reads TAMPER_N and releases INT.
  service_expanders(p_.clock.now_ms());
  sample_tamper(p_.clock.now_ms());
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
    emit(kNoSlot, EventKind::kHeartbeat, Priority::kP2Health, false,
         static_cast<std::int32_t>(p_.power.battery_mv()), static_cast<std::uint32_t>(mode_));
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
  const IdRead id = read_module_id(io, p_.board.id_eeprom_address);
  st.code = id.code;
  auto fault = [&](ModuleFault f) {
    st.fault = f;
    emit(i, EventKind::kModuleFault, Priority::kP2Health, false, static_cast<std::int32_t>(f),
         static_cast<std::uint32_t>(st.code));
  };
  const std::optional<bool> seated = p_.power.slot_present(i);
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
    case ModuleCode::kTlMod002Pir: {
      auto& d = st.driver.emplace<PirDriver>();
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
    case ModuleCode::kTmag5273Contact: {
      auto& d = st.driver.emplace<Tmag5273ContactDriver>();
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
  if (!resume) {
    emit(i, EventKind::kModulePresent, Priority::kP2Health, true,
         static_cast<std::int32_t>(st.code), 0);
  }
  return true;
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
}

// --- expanders, tamper and indicators ---------------------------------------------------

void NodeRuntime::refresh_external_power() {
  const PowerSource s = p_.power.source();
  external_power_ = s == PowerSource::kUsb || s == PowerSource::kSolarBattery;
}

void NodeRuntime::sample_tamper(TimeMs now) {
  const bool known = tamper_.state().has_value();
  if (!tamper_.sample(p_.power.tamper_open(), now)) return;
  const bool open = *tamper_.state();
  // Cold boot: a state report (b = 1), so the hub also learns that a lid it saw
  // open was closed while the node was off. Afterwards, transitions (b = 0).
  emit(kNoSlot, EventKind::kTamper, open ? Priority::kP0Critical : Priority::kP1State, open, 0,
       known ? 0 : 1);
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
    start_service_mode();
  }
}

void NodeRuntime::start_service_mode() {
  const TimeMs now = p_.clock.now_ms();
  slot_leds_.start_service(now);
  update_indicators(now);  // at once, not at the next step
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
  if (const std::uint8_t line = p_.board.status_led_line; line != 0xFF) {
    const StatusCode c = select_status(status_inputs(now));
    if (c != status_) {
      status_ = c;
      status_since_ = now;
    }
    const bool on = status_led_on(status_, now - status_since_);
    if (!status_led_known_ || on != status_led_level_) {
      status_led_known_ = p_.gpio.write(line, !on);  // active low
      status_led_level_ = on;
    }
    const TimeMs edge = status_next_edge(status_, now - status_since_);
    if (edge != kNever) led_due_ms_ = status_since_ + edge;
  }
  bool wired = false;
  for (const auto& led : p_.board.slot_led_n) wired |= led.wired();
  if (!wired) return;
  const auto modes = slot_led_modes();
  if (const TimeMs t = slot_leds_.next_change(modes, now, external_power_); t < led_due_ms_) {
    led_due_ms_ = t;
  }
  std::uint8_t want = 0;
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    if (slot_leds_.led_on(i, modes[i], now, external_power_)) {
      want = static_cast<std::uint8_t>(want | (1u << i));
    }
  }
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

PowerMode NodeRuntime::compute_mode() {
  if (p_.power.source() == PowerSource::kUsb) return PowerMode::kNormal;
  const std::uint32_t mv = p_.power.battery_mv();
  if (mv < kCriticalMv) return PowerMode::kCritical;
  if (mv < kLowPowerMv) return PowerMode::kLowPower;
  return PowerMode::kNormal;
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
    const bool power_ok =
        p_.power.source() == PowerSource::kUsb || p_.power.battery_mv() >= kOtaMinMv;
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

void NodeRuntime::send_due(TimeMs now) {
  // Nothing leaves before the epoch is persisted: the hub would drop a reused
  // (epoch, seq) as a duplicate. Events wait in the queue meanwhile.
  if (!epoch_persisted_) return;
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
    if (now < next_probe_ms_ && !p0_probe) return;
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
  if (tamper_.pending() && now >= tamper_.due_ms()) sample_tamper(now);
  poll_service_button(now);
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
    const PowerMode m = compute_mode();
    if (m != mode_) {
      mode_ = m;
      emit(kNoSlot, EventKind::kPowerMode, Priority::kP1State, false, static_cast<std::int32_t>(m),
           0);
      admit_power();
    }
    supervise_slots(now);
    emit(kNoSlot, EventKind::kHeartbeat, Priority::kP2Health, false,
         static_cast<std::int32_t>(p_.power.battery_mv()), static_cast<std::uint32_t>(mode_));
    const std::uint32_t factor = mode_ == PowerMode::kNormal ? 1u : 4u;
    next_heartbeat_ = now + static_cast<TimeMs>(config_.heartbeat_interval_ms) * factor;
  }

  if (quarantined_) retry_quarantined(now);

  // Non-critical telemetry has a shelf life: after an outage only fresh data goes out.
  queue_.remove_if([now](const DeviceEvent& e) {
    return e.priority >= Priority::kP2Health && e.at_ms + kTelemetryTtlMs <= now;
  });
  if (acks_dirty_ && now - acks_dirty_since_ >= kAckFlushMs) flush_acks();

  send_due(now);
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
  if (button_.pending() && button_.due_ms() < next) next = button_.due_ms();
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
