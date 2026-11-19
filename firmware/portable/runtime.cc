#include "firmware/portable/runtime.h"

#include <cstring>
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

void NodeRuntime::boot() {
  // Persistent boot epoch: never reuse (epoch, seq) after a reset.
  std::uint32_t epoch = 0;
  load(p_.store, StoreKey::kBootEpoch, epoch);
  boot_epoch_ = epoch + 1;
  save(p_.store, StoreKey::kBootEpoch, boot_epoch_);
  seq_ = 0;
  load(p_.store, StoreKey::kConfig, config_);

  const BootDecision d = ota_.boot();
  restore_pending_critical();
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
    SlotState& st = slots_[i];
    st = SlotState{};
    const SlotWiring& w = p_.board.slots[i];
    if (!w.present) continue;
    SlotIo io(p_, i);
    struct Deselect {  // leave the root bus free whatever happens below
      SlotIo& io;
      ~Deselect() { io.deselect(); }
    } deselect{io};
    const IdRead id = read_module_id(io, p_.board.id_eeprom_address);
    st.code = id.code;
    auto fault = [&](ModuleFault f) {
      st.fault = f;
      emit(i, EventKind::kModuleFault, Priority::kP2Health, false, static_cast<std::int32_t>(f),
           static_cast<std::uint32_t>(st.code));
    };
    switch (id.status) {
      case IdRead::Status::kAbsent:
        continue;  // nothing answered on the ID address: empty slot
      case IdRead::Status::kMuxFault:
        fault(ModuleFault::kMuxFault);
        continue;
      case IdRead::Status::kBlank:
        fault(ModuleFault::kIdBlank);  // never powered past AON
        continue;
      case IdRead::Status::kCorrupt:
        fault(ModuleFault::kIdCorrupt);
        continue;
      case IdRead::Status::kOk:
        break;
    }
    const ModuleInfo* info = find_module(st.code);
    if (!info) {
      fault(ModuleFault::kUnknownModule);  // unknown module: never powered
      continue;
    }
    if ((info->needs_wake && w.wake_line == 0xFF) || (info->needs_5v && !w.has_5v)) {
      fault(ModuleFault::kMissingResource);
      continue;
    }
    bool ok = false;
    switch (st.code) {
      case ModuleCode::kPirEkmb:
        ok = st.driver.emplace<PirDriver>().init(io, *this, now);
        break;
      case ModuleCode::kReedContact:
        ok = st.driver.emplace<ContactDriver>().init(io, *this, now);
        break;
      case ModuleCode::kTmag5273Contact:
        ok = st.driver.emplace<Tmag5273ContactDriver>().init(io, *this, now);
        break;
      case ModuleCode::kLis2dw12:
        ok = st.driver.emplace<Lis2dw12Driver>().init(io, *this, now, config_.impact_threshold_mg);
        break;
      case ModuleCode::kSht40:
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
      continue;
    }
    st.enabled = true;
    emit(i, EventKind::kModulePresent, Priority::kP2Health, true,
         static_cast<std::int32_t>(st.code), 0);
  }
}

std::uint32_t NodeRuntime::sustained_budget_mw() {
  switch (p_.power.source()) {
    case PowerSource::kUsb:
      return p_.board.usb_sustained_mw;
    case PowerSource::kSolarBattery:
      return p_.board.solar_sustained_mw;
    case PowerSource::kBatteryOnly:
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
    // Reaching the hub proves the trial image works end to end.
    if (ota_.record().state == OtaState::kPendingVerify && ota_.confirm()) {
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
  const TimeMs now = p_.clock.now_ms();
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

  if (now >= next_heartbeat_) {
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

  for (int n = 0; n < kSendsPerStep; ++n) {
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
  if (auto due = queue_.earliest_due(); due && *due < next) next = *due;
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
