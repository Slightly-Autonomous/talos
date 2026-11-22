#include "server/device_registry.h"

#include <algorithm>
#include <string>

#include "firmware/portable/hal.h"

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
    case fw::EventKind::kTamper:
      return "tamper";
    case fw::EventKind::kUserButton:
      return "user.button";
    case fw::EventKind::kConsoleLocked:
      return "console.locked";
    case fw::EventKind::kReset:
      return "device.reset";
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
    case fw::ModuleFault::kRailFault:
      return "slot switched rail overloaded and shut off";
    case fw::ModuleFault::kPowerQuarantined:
      return "slot held unpowered after a reset while powering it";
    case fw::ModuleFault::kCardRemoved:
      return "module card removed";
    case fw::ModuleFault::kWakeLineFault:
      return "module wake line never rose";
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
      t.heartbeat_seen = true;
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
    case fw::EventKind::kTamper:
      // Journaled tamper lives in the engine's state (ordered by source
      // position); only the unjournaled path below keeps a copy here.
      break;
    case fw::EventKind::kUserButton:
      if (e.a == static_cast<std::int32_t>(fw::UserButtonAction::kCommissionRequest)) {
        t.commission_requested = at;
      } else {
        t.last_test_report = at;
      }
      break;
    case fw::EventKind::kConsoleLocked:
      t.console_refusals.emplace_back(at, static_cast<std::uint32_t>(e.a > 0 ? e.a : 1));
      while (t.console_refusals.size() > kConsoleReportsPerDevice) t.console_refusals.pop_front();
      break;
    case fw::EventKind::kReset:
      // A reset through the outside USB-C port with the enclosure shut is the
      // same kind of probing as a refused console command: it counts toward
      // the DEGRADED rule (owner 2026-11-22, open-decisions "USB-C").
      if (e.a == static_cast<std::int32_t>(fw::ResetCause::kUsb) && e.b == 1) {
        t.console_refusals.emplace_back(at, 1u);
        while (t.console_refusals.size() > kConsoleReportsPerDevice) t.console_refusals.pop_front();
      }
      break;
    default:
      break;
  }
  talos::v1::Diagnostic d;
  d.set_device_id(device.str());
  d.set_at_unix_ms(at.unix_ms);
  d.set_kind(kind_name(e.kind));
  d.set_value(e.kind == fw::EventKind::kTamper        ? (e.flag ? 1 : 0)
              : e.kind == fw::EventKind::kModuleFault ? e.a
                                                      : static_cast<std::int64_t>(e.a));
  d.set_unit(e.kind == fw::EventKind::kHeartbeat ? "mV" : "");
  d.set_correlation_id(device.str() + ":" + std::to_string(e.boot_epoch) + ":" +
                       std::to_string(e.seq));
  t.recent.push_back(d);
  while (t.recent.size() > kRecentPerDevice) t.recent.pop_front();
}

bool DeviceRegistry::observe_unjournaled(const DeviceId& device, const fw::DeviceEvent& e,
                                         Timestamp at) {
  const std::pair<std::uint32_t, std::uint32_t> pos{e.boot_epoch, e.seq};
  DeviceTelemetry& t = devices_[device];
  auto& seen = t.unjournaled_seen;
  if (seen.contains(pos)) return false;
  if (seen.size() >= kSeenPerDevice && pos < *seen.begin()) return false;
  seen.insert(pos);
  while (seen.size() > kSeenPerDevice) seen.erase(seen.begin());
  // Only the newest tamper report counts: a late "closed" can't clear an open lid.
  if (e.kind == fw::EventKind::kTamper && (!t.tamper_pos || pos > *t.tamper_pos)) {
    t.tamper_open = e.flag;
    t.tamper_pos = pos;
  }
  observe(device, e, at);
  return true;
}

bool DeviceRegistry::unjournaled_is_new(const DeviceId& device, const fw::DeviceEvent& e) const {
  const auto* t = find(device);
  if (!t) return true;
  const std::pair<std::uint32_t, std::uint32_t> pos{e.boot_epoch, e.seq};
  const auto& seen = t->unjournaled_seen;
  if (seen.contains(pos)) return false;
  return !(seen.size() >= kSeenPerDevice && pos < *seen.begin());
}

void DeviceRegistry::note_behind(const DeviceId& device, Timestamp at) {
  auto& a = anomalies_[device];
  ++a.behind;
  a.last = at;
}

void DeviceRegistry::note_out_of_range(const DeviceId& device, Timestamp at) {
  auto& a = anomalies_[device];
  ++a.out_of_range;
  a.last = at;
}

const SourceAnomaly* DeviceRegistry::anomaly(const DeviceId& device) const {
  auto it = anomalies_.find(device);
  return it == anomalies_.end() ? nullptr : &it->second;
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

namespace {

// What the device's own telemetry says, past the reporting checks: OTA, power,
// slot faults. Tamper and maintenance are handled by the caller.
// TL-SN-002 built-in sensor names, by slot (firmware/esp_idf/main/board_tl_sn_002.h).
const char* built_in_name(std::uint8_t slot) {
  static const char* const kNames[] = {"magnetic contact", "vibration sensor", "motion sensor",
                                       "temperature/humidity sensor"};
  return slot < 4 ? kNames[slot] : "sensor";
}

// USB console refusals received within kConsoleRefusalWindow before `now`.
std::uint32_t recent_console_refusals(const DeviceTelemetry* t, Timestamp now) {
  if (!t) return 0;
  std::uint64_t n = 0;
  for (const auto& [at, count] : t->console_refusals) {
    if (now - at <= kConsoleRefusalWindow && at <= now) n += count;
  }
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(n, 0xFFFFFFFFu));
}

std::string console_evidence(std::uint32_t n) {
  return n >= kConsoleRefusalsDegraded
             ? "USB console access attempts while closed: " + std::to_string(n) +
                   " commands refused in 24 h"
             : std::to_string(n) + " USB console command(s) refused while closed (24 h)";
}

talos::v1::Health telemetry_findings(talos::v1::Health h, const DeviceTelemetry* t,
                                     bool primary_cells, bool built_in, Timestamp now) {
  // Console refusals are named whatever else the device reports, so a worse status
  // (low power, a fault) never hides them, and HEALTHY never does.
  const std::uint32_t refused = recent_console_refusals(t, now);
  if (refused > 0) h.add_evidence(console_evidence(refused));
  auto status = [&](talos::v1::HealthStatus s, std::string evidence, std::string action) {
    h.set_status(s);
    h.add_evidence(std::move(evidence));
    h.set_recommended_action(std::move(action));
    return h;
  };
  if (!t) return status(talos::v1::HEALTH_STATUS_HEALTHY, "reporting", "");
  if (t->ota_state && (*t->ota_state == 1 || *t->ota_state == 2 || *t->ota_state == 3)) {
    return status(talos::v1::HEALTH_STATUS_MAINTENANCE, "firmware update in progress",
                  "Leave the device powered until the update finishes");
  }
  if (t->power_mode != fw::PowerMode::kNormal) {
    if (primary_cells) {
      // AA lithium (TL-SN-002): the reading is under load, and the cells are never charged.
      return status(talos::v1::HEALTH_STATUS_LOW_POWER,
                    "AA cells " + std::to_string(t->battery_mv) + " mV under load",
                    "Replace both AA lithium cells (never recharge them), or power it from USB");
    }
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
    if (built_in) {
      return status(talos::v1::HEALTH_STATUS_FAULTED,
                    std::string("built-in ") + built_in_name(slot) + ": " + fault_name(s.fault),
                    "The node retries it on its own; if it stays faulted, replace the node");
    }
    return status(talos::v1::HEALTH_STATUS_FAULTED,
                  "slot " + std::to_string(slot) + ": " + fault_name(s.fault),
                  "Reseat or replace the module in slot " + std::to_string(slot));
  }
  if (primary_cells && t->heartbeat_seen && t->battery_mv == 0) {
    // On USB the power mode stays normal whatever the cells read (red team): a
    // node with no (or unreadable) cells is fine only until the cable comes out.
    return status(talos::v1::HEALTH_STATUS_DEGRADED,
                  "AA cells missing or unreadable (0 mV): running on USB only",
                  "Fit two fresh AA lithium cells; the node stops if USB is unplugged");
  }
  if (t->ota_state && *t->ota_state == 5) {
    h.add_evidence("last firmware update rolled back");
    degraded = true;
  }
  if (t->commission_requested) {
    // Informational: the hub does not pair on its own; the owner finishes it.
    h.add_evidence("commissioning requested from the USER button");
  }
  if (refused >= kConsoleRefusalsDegraded) {
    // Repeated mutating commands on the outside USB-C port with the lid shut: the
    // node refused them all (read-only console), but someone is trying.
    h.set_status(talos::v1::HEALTH_STATUS_DEGRADED);
    h.set_recommended_action(
        "Someone sent commands to the node's USB-C port with the enclosure closed (all refused): "
        "check the node and its cable plug, and who can reach it");
    return h;
  }
  if (degraded) {
    h.set_status(talos::v1::HEALTH_STATUS_DEGRADED);
    h.set_recommended_action("Optional features are limited; see evidence");
    return h;
  }
  return status(talos::v1::HEALTH_STATUS_HEALTHY, "reporting", "");
}

}  // namespace

talos::v1::Health project_health(const DeviceId& device, const DeviceTelemetry* t,
                                 Freshness health_freshness,
                                 std::optional<Timestamp> health_last_seen, Timestamp now,
                                 Millis offline_after, const SourceAnomaly* anomaly,
                                 bool tamper_open, std::optional<Timestamp> maintenance_until,
                                 bool primary_cells, bool built_in_sensors,
                                 const std::string& tamper_detail) {
  talos::v1::Health h;
  h.set_device_id(device.str());
  std::optional<Timestamp> seen = t && t->last_seen ? t->last_seen : health_last_seen;
  if (seen) h.set_last_seen_unix_ms(seen->unix_ms);
  // Tamper from the journal (tamper.enclosure state, any device API) or from a
  // node whose config does not declare the capability (registry only).
  const bool lid_open = tamper_open || (t && t->tamper_open.value_or(false));
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
    // Offline wins, but an enclosure last reported open is still named.
    if (lid_open) h.add_evidence("enclosure last reported open");
    return status(talos::v1::HEALTH_STATUS_OFFLINE,
                  "no report for " + std::to_string((now - *seen).count / 1000) + " s",
                  "Check power, battery and radio coverage");
  }
  if (anomaly && anomaly->last && now - *anomaly->last <= offline_after) {
    if (lid_open) h.add_evidence("enclosure open");
    return status(talos::v1::HEALTH_STATUS_FAULTED,
                  std::to_string(anomaly->behind) + " reports ignored as late, " +
                      std::to_string(anomaly->out_of_range) +
                      " refused: the hub holds a newer source position than the device sends",
                  "Check the device's credentials, then reset its sequence state "
                  "(POST /v1/devices/<id>/sequence/reset)");
  }
  // Maintenance (device.maintenance) covers the tamper alarm only. Never
  // "healthy"; any other finding (slot fault, low power, OTA) still shows,
  // with the maintenance noted.
  if (maintenance_until && now < *maintenance_until) {
    const std::string note = "maintenance for " +
                             std::to_string((*maintenance_until - now).count / 60'000) +
                             " more min: tamper alarm suppressed";
    talos::v1::Health rest = telemetry_findings(h, t, primary_cells, built_in_sensors, now);
    if (rest.status() != talos::v1::HEALTH_STATUS_HEALTHY) {
      rest.add_evidence(note);
      if (lid_open) rest.add_evidence("enclosure open");
      return rest;
    }
    h.set_status(talos::v1::HEALTH_STATUS_MAINTENANCE);
    h.add_evidence(note);
    if (lid_open) h.add_evidence("enclosure open");
    h.set_recommended_action("Close the enclosure and end maintenance when done");
    return h;
  }
  if (lid_open) {
    if (const std::uint32_t n = recent_console_refusals(t, now))
      h.add_evidence(console_evidence(n));
    if (!tamper_detail.empty()) {
      return status(talos::v1::HEALTH_STATUS_FAULTED, "tamper: " + tamper_detail,
                    "Inspect the node: close the lid and battery door, check its mount and that "
                    "no foreign magnet is near the door sensor");
    }
    return status(talos::v1::HEALTH_STATUS_FAULTED, "enclosure tamper: lid open or loop cut",
                  "Check the enclosure lid and its tamper switch wiring (J9)");
  }
  return telemetry_findings(h, t, primary_cells, built_in_sensors, now);
}

bool board_has_primary_cells(const std::optional<BoardId>& board) {
  return board && *board == BoardId("sensor_node_v2");
}

bool board_has_built_in_sensors(const std::optional<BoardId>& board) {
  return board && *board == BoardId("sensor_node_v2");
}

}  // namespace talos::server
