#include "server/device_registry.h"

namespace talos::server {

namespace {

std::string kind_name(fw::EventKind k) {
  switch (k) {
    case fw::EventKind::kMotion:
      return "motion";
    case fw::EventKind::kContact:
      return "contact";
    case fw::EventKind::kImpact:
      return "impact";
    case fw::EventKind::kEnvironment:
      return "environment";
    case fw::EventKind::kHeartbeat:
      return "heartbeat.battery";
    case fw::EventKind::kModuleFault:
      return "module.fault";
    case fw::EventKind::kModulePresent:
      return "module.present";
    case fw::EventKind::kConfigApplied:
      return "config.applied";
    case fw::EventKind::kOtaStatus:
      return "ota.state";
    case fw::EventKind::kPowerMode:
      return "power.mode";
  }
  return "unknown";
}

const char* fault_name(fw::ModuleFault f) {
  switch (f) {
    case fw::ModuleFault::kNone:
      return "ok";
    case fw::ModuleFault::kUnknownModule:
      return "unknown module";
    case fw::ModuleFault::kNoIdEeprom:
      return "no module id";
    case fw::ModuleFault::kInitFailed:
      return "module failed to start";
    case fw::ModuleFault::kBusError:
      return "module bus error";
    case fw::ModuleFault::kDisabledForPower:
      return "disabled to save power";
    case fw::ModuleFault::kMissingResource:
      return "slot cannot host this module";
    case fw::ModuleFault::kMuxFault:
      return "slot bus switch fault";
    case fw::ModuleFault::kIdBlank:
      return "module id not programmed";
    case fw::ModuleFault::kIdCorrupt:
      return "module id corrupt";
    case fw::ModuleFault::kStuckInterrupt:
      return "module interrupt stuck";
    case fw::ModuleFault::kWarmupTimeout:
      return "module never settled after power-up";
  }
  return "fault";
}

}  // namespace

void DeviceRegistry::observe(const DeviceId& device, const fw::DeviceEvent& e, Timestamp at) {
  DeviceTelemetry& t = devices_[device];
  t.last_seen = at;
  switch (e.kind) {
    case fw::EventKind::kHeartbeat:
      t.battery_mv = static_cast<std::uint32_t>(e.a);
      t.power_mode = static_cast<fw::PowerMode>(e.b);
      break;
    case fw::EventKind::kPowerMode:
      t.power_mode = static_cast<fw::PowerMode>(e.a);
      break;
    case fw::EventKind::kModulePresent:
      t.slots[e.slot] = {static_cast<std::uint16_t>(e.a), fw::ModuleFault::kNone};
      break;
    case fw::EventKind::kModuleFault:
      t.slots[e.slot] = {static_cast<std::uint16_t>(e.b), static_cast<fw::ModuleFault>(e.a)};
      break;
    case fw::EventKind::kOtaStatus:
      t.ota_state = static_cast<std::uint8_t>(e.a);
      if (e.a == 1) {  // downloading: b is the next byte wanted
        t.ota_received = e.b;
      } else {
        t.running_version = e.b;
      }
      break;
    case fw::EventKind::kConfigApplied:
      t.applied_config_revision = static_cast<std::uint32_t>(e.a);
      break;
    default:
      break;
  }
  talos::v1::Diagnostic d;
  d.set_device_id(device.str());
  d.set_at_unix_ms(at.unix_ms);
  d.set_kind(kind_name(e.kind));
  d.set_value(e.kind == fw::EventKind::kModuleFault ? e.a : static_cast<std::int64_t>(e.a));
  d.set_unit(e.kind == fw::EventKind::kHeartbeat ? "mV" : "");
  d.set_correlation_id(device.str() + ":" + std::to_string(e.boot_epoch) + ":" +
                       std::to_string(e.seq));
  t.recent.push_back(d);
  while (t.recent.size() > kRecentPerDevice) t.recent.pop_front();
}

const DeviceTelemetry* DeviceRegistry::find(const DeviceId& device) const {
  auto it = devices_.find(device);
  return it == devices_.end() ? nullptr : &it->second;
}

std::vector<talos::v1::Diagnostic> DeviceRegistry::diagnostics(const DeviceId& device) const {
  const auto* t = find(device);
  if (!t) return {};
  return {t->recent.begin(), t->recent.end()};
}

talos::v1::Health project_health(const DeviceId& device, const DeviceTelemetry* t,
                                 Freshness health_freshness,
                                 std::optional<Timestamp> health_last_seen, Timestamp now,
                                 Millis offline_after) {
  talos::v1::Health h;
  h.set_device_id(device.str());
  std::optional<Timestamp> seen = t && t->last_seen ? t->last_seen : health_last_seen;
  if (seen) h.set_last_seen_unix_ms(seen->unix_ms);
  auto status = [&](talos::v1::HealthStatus s, std::string evidence, std::string action) {
    h.set_status(s);
    h.add_evidence(std::move(evidence));
    h.set_recommended_action(std::move(action));
    return h;
  };
  if (!seen) {
    return status(talos::v1::HEALTH_STATUS_OFFLINE, "never reported",
                  "Check power and that the device is paired");
  }
  const bool stale = (now - *seen) > offline_after || (!t && health_freshness != Freshness::kFresh);
  if (stale) {
    return status(talos::v1::HEALTH_STATUS_OFFLINE,
                  "no report for " + std::to_string((now - *seen).count / 1000) + " s",
                  "Check power, battery and radio coverage");
  }
  if (!t) return status(talos::v1::HEALTH_STATUS_HEALTHY, "reporting", "");
  if (t->ota_state && (*t->ota_state == 1 || *t->ota_state == 2 || *t->ota_state == 3)) {
    return status(talos::v1::HEALTH_STATUS_MAINTENANCE, "firmware update in progress",
                  "Leave the device powered until the update finishes");
  }
  if (t->power_mode != fw::PowerMode::kNormal) {
    return status(talos::v1::HEALTH_STATUS_LOW_POWER,
                  "battery " + std::to_string(t->battery_mv) + " mV",
                  "Charge or replace the battery, or add solar/USB power");
  }
  bool degraded = false;
  for (const auto& [slot, s] : t->slots) {
    if (s.fault == fw::ModuleFault::kNone) continue;
    if (s.fault == fw::ModuleFault::kDisabledForPower) {
      degraded = true;
      h.add_evidence("slot " + std::to_string(slot) + ": " + fault_name(s.fault));
      continue;
    }
    return status(talos::v1::HEALTH_STATUS_FAULTED,
                  "slot " + std::to_string(slot) + ": " + fault_name(s.fault),
                  "Reseat or replace the module in slot " + std::to_string(slot));
  }
  if (t->ota_state && *t->ota_state == 5) {
    h.add_evidence("last firmware update rolled back");
    degraded = true;
  }
  if (degraded) {
    h.set_status(talos::v1::HEALTH_STATUS_DEGRADED);
    h.set_recommended_action("Optional features are limited; see evidence");
    return h;
  }
  return status(talos::v1::HEALTH_STATUS_HEALTHY, "reporting", "");
}

}  // namespace talos::server
