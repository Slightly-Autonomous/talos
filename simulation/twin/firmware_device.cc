#include "simulation/twin/firmware_device.h"

#include <algorithm>
#include <map>

#include "firmware/bridge/hub_bridge.h"

namespace talos::sim {

namespace {

// Site module ids the firmware's registry knows (firmware/portable/modules.h).
const std::map<std::string, fw::ModuleCode>& firmware_modules() {
  static const auto* m = new std::map<std::string, fw::ModuleCode>{
      {"pir_ekmb_v1", fw::ModuleCode::kPirEkmb},
      {"reed_contact_v1", fw::ModuleCode::kReedContact},
      {"vibration_lis2dw12_v1", fw::ModuleCode::kLis2dw12},
      {"environment_sht40_v1", fw::ModuleCode::kSht40},
  };
  return *m;
}

}  // namespace

std::string FirmwareDevice::unsupported_reason(const v1::SiteDevice& spec) {
  if (spec.has_speaker() || spec.has_lock() || spec.has_notifier()) {
    return "the firmware has no actuators";
  }
  std::optional<std::uint32_t> threshold;
  for (const auto& s : spec.sensors()) {
    if (s.has_camera()) return "the firmware has no camera module";
    if (s.slot() >= fw::kSlots) return "slot " + std::to_string(s.slot()) + " does not exist";
    if (!firmware_modules().contains(s.module_id())) {
      return "module " + s.module_id() + " is not in the firmware registry";
    }
    if (s.has_impact()) {
      // One LIS2DW12 wake-up threshold per node (RuntimeConfig).
      if (threshold && *threshold != s.impact().threshold_mg()) {
        return "the firmware has one impact threshold per node";
      }
      threshold = s.impact().threshold_mg();
    }
  }
  return "";
}

FirmwareDevice::FirmwareDevice(const v1::SiteDevice& spec, PropertyId property, std::uint64_t seed)
    : id_(spec.id()),
      device_(spec.id()),
      property_(std::move(property)),
      position_(to_vec(spec.position())),
      rng_(Rng::stream(seed, spec.id())),
      node_(std::make_unique<fw::sim::Node>(fw::sim::default_board())) {
  if (spec.battery_mv() > 0) {
    node_->power.battery_mv_ = spec.battery_mv();
    node_->power.source_ = fw::PowerSource::kBatteryOnly;
  }
  config_.revision = 1;
  config_.heartbeat_interval_ms = spec.heartbeat_interval_ms();
  for (const auto& s : spec.sensors()) {
    const auto slot = static_cast<std::uint8_t>(s.slot());
    node_->insert(slot, firmware_modules().at(s.module_id()));
    const std::uint8_t line = node_->board.slots[slot].wake_line;
    if (s.has_pir()) pirs_.push_back({line, PirOptics(position_, s.pir())});
    if (s.has_contact()) contacts_.push_back({line, s.contact().portal_id()});
    if (s.has_impact()) {
      impacts_.push_back({slot, s.impact()});
      config_.impact_threshold_mg = s.impact().threshold_mg();
    }
    if (s.has_environment()) {
      environments_.push_back({slot, s.environment()});
      config_.environment_interval_ms = s.environment().report_interval_ms();
    }
  }
}

void FirmwareDevice::apply_inputs(const World& world) {
  for (const auto& c : contacts_) {
    if (const Portal* p = world.portal(c.portal)) node_->gpio.set(c.line, p->open);
  }
  const auto zone = world.zone_at(position_).value_or("");
  for (const auto& e : environments_) {
    node_->sht(e.slot).temperature_c = (world.temperature(zone) + e.spec.bias_milli_c()) / 1000.0;
  }
}

void FirmwareDevice::power_on(Timestamp now, const World& world) {
  start_ = now - Millis{static_cast<std::int64_t>(node_->clock.now_ms())};
  for (auto& p : pirs_) {
    p.optics.reset();
    node_->gpio.set(p.line, false);
  }
  apply_inputs(world);
  node_->boot();
  // The hub's per-device config (the API pushes the same ConfigUpdate on the wire). The
  // firmware keeps it across reboots and ignores a revision it already has.
  node_->link.downlink.push_back(config_);
}

void FirmwareDevice::boot(std::int64_t t_ms, Timestamp now, const World& world,
                          std::vector<Uplink>& out) {
  last_t_ms_ = t_ms;
  // Installed earlier: run the firmware through its warm-up before the scenario starts (the
  // PIR mask needs 30 s of quiet input). What it sent meanwhile is ACKed here, so it is not
  // retried, and reaches the hub once at t_ms, as a node joining the hub reports its state.
  constexpr fw::TimeMs kPreRollMs = fw::PirDriver::kWarmMinMs + 1'000;
  power_on(now - Millis{static_cast<std::int64_t>(kPreRollMs)}, world);
  std::vector<fw::DeviceEvent> sent;
  for (fw::TimeMs ms = 0; ms < kPreRollMs; ms += 10) {
    node_->run_for(10);
    for (const auto& e : node_->link.uplink) {
      const bool seen = std::any_of(sent.begin(), sent.end(), [&](const fw::DeviceEvent& x) {
        return x.boot_epoch == e.boot_epoch && x.seq == e.seq;
      });
      if (!seen) sent.push_back(e);
      node_->link.downlink.push_back(fw::Ack{e.boot_epoch, e.seq});
    }
    node_->link.uplink.clear();
  }
  start_ = now - Millis{static_cast<std::int64_t>(node_->clock.now_ms())};
  // Only the latest report per (slot, kind) still describes the device; older ones were
  // superseded during the pre-roll.
  for (std::size_t i = 0; i < sent.size(); ++i) {
    const bool superseded =
        std::any_of(sent.begin() + static_cast<std::ptrdiff_t>(i) + 1, sent.end(),
                    [&](const fw::DeviceEvent& later) {
                      return later.slot == sent[i].slot && later.kind == sent[i].kind;
                    });
    if (!superseded) node_->link.uplink.push_back(sent[i]);
  }
  drain(out);
}

void FirmwareDevice::sample(std::int64_t t_ms, Timestamp, World& world,
                            const std::vector<std::pair<std::string, std::uint32_t>>& impacts,
                            std::vector<Uplink>& out) {
  apply_inputs(world);
  for (auto& p : pirs_) node_->gpio.set(p.line, p.optics.step(t_ms, world, rng_));
  for (const auto& imp : impacts_) {
    for (const auto& [portal, mg] : impacts) {
      if (portal != imp.spec.portal_id()) continue;
      node_->imu(imp.slot).impact(static_cast<std::int32_t>(mg * imp.spec.coupling()));
    }
  }
  if (t_ms > last_t_ms_) node_->run_for(static_cast<fw::TimeMs>(t_ms - last_t_ms_));
  last_t_ms_ = t_ms;
  drain(out);
}

void FirmwareDevice::drain(std::vector<Uplink>& out) {
  auto& up = node_->link.uplink;
  while (!up.empty()) {
    const fw::DeviceEvent e = up.front();
    up.pop_front();
    if (auto env = fw::to_envelope(e, device_, property_)) {
      env->observed_at = start_ + Millis{static_cast<std::int64_t>(e.at_ms) + skew_ms_};
      out.push_back({id_, std::move(*env)});
    } else {
      // Diagnostics (module present, config applied, ...) belong to the observability
      // plane; the hub ACKs them on the wire so they are not retried.
      node_->link.downlink.push_back(fw::Ack{e.boot_epoch, e.seq});
    }
  }
}

void FirmwareDevice::receive(std::int64_t t_ms, const Command& command, World& world) {
  world.record(t_ms, "device.unsupported_command", id_, command.id.str());
}

void FirmwareDevice::delivered(const SourcePosition& position) {
  node_->link.downlink.push_back(
      fw::Ack{position.boot_epoch, static_cast<std::uint32_t>(position.sequence)});
}

void FirmwareDevice::reboot(std::int64_t t_ms, Timestamp now, const World& world,
                            std::vector<Uplink>& out) {
  // A reset loses whatever the firmware had not persisted, exactly as on the board.
  node_->link.downlink.clear();
  last_t_ms_ = t_ms;
  power_on(now, world);
  node_->run_for(1);
  drain(out);
}

}  // namespace talos::sim
