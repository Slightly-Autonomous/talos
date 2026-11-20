#include "firmware/portable/runtime.h"

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

struct PendingBlob {
  std::uint32_t count = 0;
  DeviceEvent events[NodeRuntime::kMaxPendingCritical];
};

}  // namespace

void NodeRuntime::emit(std::uint8_t slot, EventKind kind, Priority priority, bool flag,
                       std::int32_t a, std::uint32_t b) {
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
  queue_.push(e);
  if (priority == Priority::kP0Critical) persist_pending_critical();
}

void NodeRuntime::persist_pending_critical() {
  PendingBlob blob;
  queue_.for_each([&](const DeviceEvent& e) {
    if (e.priority == Priority::kP0Critical && blob.count < kMaxPendingCritical) {
      blob.events[blob.count++] = e;
    }
  });
  save(p_.store, StoreKey::kPendingCritical, blob);
}

void NodeRuntime::restore_pending_critical() {
  PendingBlob blob;
  if (!load(p_.store, StoreKey::kPendingCritical, blob)) return;
  for (std::uint32_t i = 0; i < blob.count && i < kMaxPendingCritical; ++i) {
    queue_.push(blob.events[i]);  // keeps its original (boot_epoch, seq)
  }
}

bool NodeRuntime::persist_boot_epoch() {
  if (boot_epoch_ == kEpochExhausted) return false;  // fail loudly: never send again
  return save(p_.store, StoreKey::kBootEpoch, boot_epoch_);
}

void NodeRuntime::boot() {
  feed_watchdog();
  // Persistent boot epoch: strictly increasing, never reuse (epoch, seq) after a
  // reset. Until it is written nothing is transmitted (step() retries).
  std::uint32_t epoch = 0;
  load(p_.store, StoreKey::kBootEpoch, epoch);
  boot_epoch_ = epoch >= kEpochExhausted - 1 ? kEpochExhausted : epoch + 1;
  epoch_persisted_ = persist_boot_epoch() || persist_boot_epoch();
  next_epoch_retry_ = p_.clock.now_ms() + kEpochRetryMs;
  seq_ = 0;
  load(p_.store, StoreKey::kConfig, config_);

  const BootDecision d = ota_.boot();
  restore_pending_critical();
  board_ok_ = p_.power.begin();  // safe outputs before anything touches a slot
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
  const TimeMs now = p_.clock.now_ms();
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    quarantine_retry_at_[i] = now + quarantine_backoff_ms(strikes_[i]);
  }
  aon_on_ = 0;
  mode_ = compute_mode();
  discover();
  admit_power();

  // Boot status tells the hub the running version (and any rollback).
  (void)d;
  emit_ota_status(Priority::kP1State);
  emit(kNoSlot, EventKind::kHeartbeat, Priority::kP2Health, false,
       static_cast<std::int32_t>(p_.power.battery_mv()), static_cast<std::uint32_t>(mode_));
  next_heartbeat_ = p_.clock.now_ms() + config_.heartbeat_interval_ms;
}

void NodeRuntime::emit_ota_status(Priority priority) {
  const OtaRecord& r = ota_.record();
  // b: next wanted byte while downloading, otherwise the running version.
  const std::uint32_t b = r.state == OtaState::kDownloading ? r.received : ota_.running_version();
  emit(kNoSlot, EventKind::kOtaStatus, priority, false, static_cast<std::int32_t>(r.state), b);
}

void NodeRuntime::discover() {
  const TimeMs now = p_.clock.now_ms();
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    discover_slot(i, now);
    feed_watchdog();  // each slot can take tens of ms (rail settle, NVS, sensor probes)
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
}

void NodeRuntime::discover_slot(std::uint8_t i, TimeMs now) {
  SlotState& st = slots_[i];
  st = SlotState{};
  const SlotWiring& w = p_.board.slots[i];
  if (!w.present) return;
  if (!power_slot(i)) return;
  bool keep_powered = true;
  {
    SlotIo io(p_, i);
    keep_powered = init_slot(i, io, now);
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

bool NodeRuntime::init_slot(std::uint8_t i, SlotIo& io, TimeMs now) {
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
  bool ok = false;
  switch (st.code) {
    case ModuleCode::kPirEkmb:
    case ModuleCode::kTlMod002Pir:
      ok = st.driver.emplace<PirDriver>().init(io, *this, now);
      break;
    case ModuleCode::kTlTest001:
      ok = st.driver.emplace<TestCardDriver>().init(io, *this, now);
      break;
    case ModuleCode::kReedContact:
      ok = st.driver.emplace<ContactDriver>().init(io, *this, now);
      break;
    case ModuleCode::kTmag5273Contact:
      ok = st.driver.emplace<Tmag5273ContactDriver>().init(io, *this, now);
      break;
    case ModuleCode::kLis2dw12:
    case ModuleCode::kTlMod003Imu:
      ok = st.driver.emplace<Lis2dw12Driver>().init(io, *this, now, config_.impact_threshold_mg);
      break;
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
  emit(i, EventKind::kModulePresent, Priority::kP2Health, true, static_cast<std::int32_t>(st.code),
       0);
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

PowerMode NodeRuntime::compute_mode() {
  if (p_.power.source() == PowerSource::kUsb) return PowerMode::kNormal;
  const std::uint32_t mv = p_.power.battery_mv();
  if (mv < kCriticalMv) return PowerMode::kCritical;
  if (mv < kLowPowerMv) return PowerMode::kLowPower;
  return PowerMode::kNormal;
}

void NodeRuntime::handle(const Downlink& d) {
  if (const auto* ack = std::get_if<Ack>(&d)) {
    const bool was_critical_pending = queue_.ack(ack->boot_epoch, ack->seq);
    if (was_critical_pending) persist_pending_critical();
    // Health check for a trial image: it reached the hub end to end, and the
    // board took its safe state at boot. Only then is it marked valid (here and
    // in the bootloader); otherwise the next reboot rolls it back.
    if (ota_.record().state == OtaState::kPendingVerify && board_ok_ && ota_.confirm()) {
      emit_ota_status(Priority::kP1State);
    }
  } else if (const auto* cfg = std::get_if<ConfigUpdate>(&d)) {
    if (cfg->revision <= config_.revision) return;  // stale or duplicate
    RuntimeConfig next = config_;
    next.revision = cfg->revision;
    if (cfg->heartbeat_interval_ms >= 10'000)
      next.heartbeat_interval_ms = cfg->heartbeat_interval_ms;
    if (cfg->environment_interval_ms >= 10'000)
      next.environment_interval_ms = cfg->environment_interval_ms;
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
    if (ota_.reboot_wanted()) reboot_requested_ = true;
  } else if (std::holds_alternative<RebootRequest>(d)) {
    reboot_requested_ = true;
  }
}

void NodeRuntime::step() {
  feed_watchdog();
  const TimeMs now = p_.clock.now_ms();
  if (!epoch_persisted_ && now >= next_epoch_retry_) {
    epoch_persisted_ = persist_boot_epoch();
    next_epoch_retry_ = now + kEpochRetryMs;
  }
  while (auto d = p_.link.receive()) handle(*d);
  // One progress report per batch of chunks: the hub streams the next ones
  // in its reply (device-initiated exchange, sleepy-node friendly).
  if (ota_progress_ && ota_.record().state == OtaState::kDownloading) {
    emit_ota_status(Priority::kP2Health);
  }
  ota_progress_ = false;

  const std::uint32_t env_interval =
      config_.environment_interval_ms * (mode_ == PowerMode::kNormal ? 1u : 4u);
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    SlotState& st = slots_[i];
    if (!st.enabled) continue;
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
    const PowerMode m = compute_mode();
    if (m != mode_) {
      mode_ = m;
      emit(kNoSlot, EventKind::kPowerMode, Priority::kP1State, false, static_cast<std::int32_t>(m),
           0);
      admit_power();
    }
    emit(kNoSlot, EventKind::kHeartbeat, Priority::kP2Health, false,
         static_cast<std::int32_t>(p_.power.battery_mv()), static_cast<std::uint32_t>(mode_));
    const std::uint32_t factor = mode_ == PowerMode::kNormal ? 1u : 4u;
    next_heartbeat_ = now + static_cast<TimeMs>(config_.heartbeat_interval_ms) * factor;
  }

  if (quarantined_) retry_quarantined(now);

  // Nothing leaves before the epoch is persisted: the hub would drop a reused
  // (epoch, seq) as a duplicate. Events wait in the queue meanwhile.
  for (int n = 0; epoch_persisted_ && n < kSendsPerStep; ++n) {
    auto* s = queue_.next_due(now);
    if (!s) break;
    p_.link.send(s->event);
    queue_.mark_sent(*s, now);  // backoff whether or not the radio took it
  }
  awake_ = false;
}

TimeMs NodeRuntime::next_wake_ms() const {
  const TimeMs now = p_.clock.now_ms();
  TimeMs next = next_heartbeat_;
  if (!epoch_persisted_) {
    if (next_epoch_retry_ < next) next = next_epoch_retry_;
  } else if (auto due = queue_.earliest_due(); due && *due < next) {
    next = *due;
  }
  if (quarantined_) {
    for (std::uint8_t i = 0; i < kSlots; ++i) {
      if ((quarantined_ & (1u << i)) && quarantine_retry_at_[i] < next) {
        next = quarantine_retry_at_[i];
      }
    }
  }
  for (const auto& st : slots_) {
    if (!st.enabled) continue;
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
