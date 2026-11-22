#include "simulation/runner/runner.h"

#include <google/protobuf/util/json_util.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

#include "codec/codec.h"
#include "codec/yaml_loader.h"
#include "core/engine/engine.h"
#include "simulation/twin/devices.h"
#include "simulation/twin/firmware_device.h"
#include "simulation/twin/transport.h"
#include "simulation/twin/world.h"

namespace talos::sim {

namespace pb = ::talos::v1;

namespace {

std::string json_escape(const std::string& s) {
  std::string out;
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += ' ';
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string capability_for(const v1::SensorSpec& s) {
  switch (s.kind_case()) {
    case v1::SensorSpec::kPir:
      return "motion.detect";
    case v1::SensorSpec::kContact:
      return "contact.open_state";
    case v1::SensorSpec::kImpact:
      return "impact.detect";
    case v1::SensorSpec::kEnvironment:
      return "environment.temperature_humidity";
    case v1::SensorSpec::kCamera:
      return "camera.detection";
    case v1::SensorSpec::KIND_NOT_SET:
      break;
  }
  return "";
}

Property build_property(const v1::Site& site, const World& world) {
  Property p;
  p.id = PropertyId(site.property_id());
  p.timezone = site.timezone();
  for (const auto& z : site.zones()) {
    p.zones.push_back({ZoneId(z.id()), z.name(),
                       z.kind() == pb::ZONE_KIND_OUTDOOR ? ZoneKind::kOutdoor : ZoneKind::kIndoor});
  }
  std::map<std::string, Asset> assets;
  for (const auto& portal : site.portals()) {
    Asset a;
    a.id = AssetId(portal.id());
    a.type = portal.type();
    if (!portal.zone_id().empty()) a.zone = ZoneId(portal.zone_id());
    assets[portal.id()] = a;
  }
  for (const auto& d : site.devices()) {
    Device dev;
    dev.id = DeviceId(d.id());
    dev.property = p.id;
    if (d.has_board_id()) dev.board = BoardId(d.board_id());
    if (auto zone = world.zone_at(to_vec(d.position()))) dev.zone = ZoneId(*zone);
    // Declare what the twin device reports or is commanded on: an empty list
    // declares nothing (fail closed).
    auto declare = [&](const std::string& c) {
      if (!c.empty() && std::find(dev.capabilities.begin(), dev.capabilities.end(),
                                  CapabilityId(c)) == dev.capabilities.end()) {
        dev.capabilities.emplace_back(c);
      }
    };
    if (d.has_lock()) declare("access.lock");
    if (d.has_speaker()) declare("audio.speak");
    if (d.has_notifier()) declare("notify");
    if (d.has_enclosure()) declare("tamper.enclosure");
    for (const auto& s : d.sensors()) {
      declare(capability_for(s));
      if (s.slot() < kMaxSlots) dev.slots[s.slot()] = ModuleId(s.module_id());
      const std::string* portal = nullptr;
      if (s.has_contact()) portal = &s.contact().portal_id();
      if (s.has_impact()) portal = &s.impact().portal_id();
      if (portal && assets.contains(*portal)) {
        assets[*portal].attached.push_back(
            {dev.id, static_cast<SlotIndex>(s.slot()), CapabilityId(capability_for(s))});
      }
    }
    if (d.has_lock() && assets.contains(d.lock().portal_id())) {
      assets[d.lock().portal_id()].attached.push_back(
          {dev.id, std::nullopt, CapabilityId("access.lock")});
    }
    p.devices.push_back(dev);
  }
  for (auto& [id, a] : assets) p.assets.push_back(std::move(a));
  return p;
}

Result<EngineConfig> build_config(const v1::Scenario& sc, const v1::Site& site,
                                  const World& world) {
  EngineConfig cfg;
  cfg.property = build_property(site, world);
  if (Status s = validate_property(cfg.property); !s) return s.error();
  cfg.freshness_max_age =
      Millis{sc.freshness_max_age_ms() > 0 ? sc.freshness_max_age_ms() : 15 * 60 * 1000};
  for (const auto& r : sc.rules()) {
    auto rule = codec::from_proto(r);
    if (!rule) return rule.error();
    cfg.rules.push_back(rule.value());
  }
  auto risk = [](pb::RiskClass r) {
    switch (r) {
      case pb::RISK_CLASS_SENSITIVE_READ:
        return RiskClass::kSensitiveRead;
      case pb::RISK_CLASS_BENIGN_OUTPUT:
        return RiskClass::kBenignOutput;
      case pb::RISK_CLASS_DISRUPTIVE_OUTPUT:
        return RiskClass::kDisruptiveOutput;
      case pb::RISK_CLASS_SECURITY_CRITICAL:
        return RiskClass::kSecurityCritical;
      default:
        return RiskClass::kInformational;
    }
  };
  for (const auto& p : sc.principals()) {
    PrincipalKind kind = PrincipalKind::kHuman;
    if (p.kind() == v1::PRINCIPAL_KIND_AGENT) kind = PrincipalKind::kAgent;
    if (p.kind() == v1::PRINCIPAL_KIND_RULE) kind = PrincipalKind::kRule;
    cfg.principals.push_back({PrincipalId(p.id()), kind, {{cfg.property.id, risk(p.max_risk())}}});
  }
  // Rules act under their own principal; add one if the scenario did not.
  for (const auto& rule : cfg.rules) {
    const bool listed = std::any_of(cfg.principals.begin(), cfg.principals.end(),
                                    [&](const Principal& p) { return p.id == rule.principal; });
    if (!listed) {
      cfg.principals.push_back({rule.principal,
                                PrincipalKind::kRule,
                                {{cfg.property.id, RiskClass::kDisruptiveOutput}}});
    }
  }
  const std::int64_t offset = site.utc_offset_minutes();
  cfg.local_minute_of_day = [offset](Timestamp t) -> std::uint16_t {
    std::int64_t minutes = t.unix_ms / 60'000 + offset;
    minutes %= 1440;
    if (minutes < 0) minutes += 1440;
    return static_cast<std::uint16_t>(minutes);
  };
  return cfg;
}

Freshness to_core(v1::Freshness f) {
  switch (f) {
    case v1::FRESHNESS_FRESH:
      return Freshness::kFresh;
    case v1::FRESHNESS_STALE:
      return Freshness::kStale;
    default:
      return Freshness::kUnknown;
  }
}

// "COMMAND_STATUS_VERIFIED" matches core "verified".
bool status_matches(CommandStatus status, int proto_status) {
  std::string name = pb::CommandStatus_Name(static_cast<pb::CommandStatus>(proto_status));
  const std::string prefix = "COMMAND_STATUS_";
  if (name.rfind(prefix, 0) == 0) name = name.substr(prefix.size());
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return name == to_string(status);
}

std::string_view freshness_name(Freshness f) {
  switch (f) {
    case Freshness::kFresh:
      return "fresh";
    case Freshness::kStale:
      return "stale";
    case Freshness::kUnknown:
      return "unknown";
  }
  return "?";
}

class Run {
 public:
  Run(const v1::Scenario& sc, const v1::Site& site, RunOptions options)
      : sc_(sc),
        site_(site),
        options_(options),
        start_(Timestamp{sc.start_unix_ms()}),
        clock_(start_),
        world_(site),
        transport_(sc.links(), sc.seed()) {}

  ScenarioReport execute() {
    report_.name = sc_.name();
    report_.seed = sc_.seed();
    auto cfg = build_config(sc_, site_, world_);
    if (!cfg) return fail_early("invalid config: " + cfg.error().message);
    config_ = cfg.value();
    engine_ = std::make_unique<HubEngine>(config_, journal_, clock_);
    if (!bind_devices()) return report_;

    std::vector<const v1::Stimulus*> stimuli;
    for (const auto& s : sc_.stimuli()) stimuli.push_back(&s);
    std::stable_sort(stimuli.begin(), stimuli.end(),
                     [](const auto* a, const auto* b) { return a->at_ms() < b->at_ms(); });
    std::size_t next = 0;

    std::vector<Uplink> up;
    for (auto& [id, dev] : devices_) dev->boot(0, clock_.now(), world_, up);
    send_uplinks(0, up);

    for (std::int64_t t = 0; t <= sc_.duration_ms(); t += kStepMs) {
      clock_.set(start_ + Millis{t});
      while (next < stimuli.size() && stimuli[next]->at_ms() <= t) apply(t, *stimuli[next++]);
      world_.step(t, kStepMs / 1000.0);
      const auto impacts = world_.take_impacts();
      up.clear();
      for (auto& [id, dev] : devices_) dev->sample(t, clock_.now(), world_, impacts, up);
      send_uplinks(t, up);
      for (auto& m : transport_.due(t)) deliver(t, m);
      if (!engine_->tick()) report_.failures.push_back("journal append failed during tick");
      for (auto& cmd : engine_->take_outbound()) {
        transport_.send(t, cmd.target.device.str(), false, cmd);
      }
    }
    evaluate();
    check_invariants();
    finish();
    return report_;
  }

 private:
  // Chooses each device's backend. False (and a skipped report) when a device is bound to
  // one that cannot play it.
  bool bind_devices() {
    bool any_firmware = false;
    for (const auto& d : site_.devices()) {
      auto binding = d.binding();
      const bool explicit_binding = binding != v1::DEVICE_BINDING_UNSPECIFIED;
      if (!explicit_binding) {
        binding = options_.backend == Backend::kFirmware ? v1::DEVICE_BINDING_FIRMWARE
                                                         : v1::DEVICE_BINDING_BEHAVIOURAL;
      }
      if (binding == v1::DEVICE_BINDING_HARDWARE) {
        return skip(d.id() + " is bound to hardware (L4), which the twin cannot run yet");
      }
      if (binding == v1::DEVICE_BINDING_FIRMWARE) {
        const std::string why = FirmwareDevice::unsupported_reason(d);
        if (why.empty()) {
          devices_.emplace(d.id(),
                           std::make_unique<FirmwareDevice>(d, config_.property.id, sc_.seed()));
          any_firmware = true;
        } else if (explicit_binding) {
          return skip(d.id() + " is bound to firmware (L2), but " + why);
        }
      }
      if (!devices_.contains(d.id())) {
        devices_.emplace(d.id(),
                         std::make_unique<VirtualDevice>(d, config_.property.id, sc_.seed()));
      }
      report_.bindings.emplace_back(d.id(), devices_.at(d.id())->level());
    }
    if (options_.backend == Backend::kFirmware && !any_firmware) {
      return skip("no device in this site can run on the firmware (L2)");
    }
    return true;
  }

  bool skip(std::string why) {
    report_.skipped = true;
    report_.skip_reason = std::move(why);
    report_.bindings.clear();
    finish();
    report_.passed = false;
    return false;
  }

  ScenarioReport fail_early(std::string why) {
    report_.failures.push_back(std::move(why));
    finish();
    return report_;
  }

  void send_uplinks(std::int64_t t, std::vector<Uplink>& up) {
    for (auto& u : up) {
      if (auto* env = std::get_if<ObservationEnvelope>(&u.body)) {
        transport_.send(t, u.device, true, *env);
      } else if (auto* d = std::get_if<DeviceDiagnostic>(&u.body)) {
        // Observability plane: recorded when the device sends it, never journaled.
        diagnostics_.push_back({t, u.device, d->kind, d->value});
        ++report_.diagnostics[u.device + " " + d->kind];
        world_.record(t, "diagnostic." + d->kind, u.device, std::to_string(d->value));
      } else {
        transport_.send(t, u.device, true, std::get<CommandAck>(u.body));
      }
    }
    up.clear();
  }

  void deliver(std::int64_t t, Message& m) {
    auto it = devices_.find(m.device);
    if (!m.uplink) {
      if (it != devices_.end()) it->second->receive(t, std::get<Command>(m.body), world_);
      return;
    }
    if (const auto* env = std::get_if<ObservationEnvelope>(&m.body)) {
      const auto r = engine_->on_observation(*env);
      if (r.status != IngestStatus::kRejected && it != devices_.end()) {
        it->second->delivered(env->position);
      }
    } else if (const auto* ack = std::get_if<CommandAck>(&m.body)) {
      (void)engine_->on_ack(*ack);
    }
  }

  void apply(std::int64_t t, const v1::Stimulus& s) {
    std::vector<Uplink> up;
    switch (s.kind_case()) {
      case v1::Stimulus::kActorEnter: {
        const auto& e = s.actor_enter();
        Actor a;
        a.id = e.actor();
        a.kind = e.kind() == v1::ACTOR_KIND_PET ? ActorKind::kPet : ActorKind::kPerson;
        a.position = to_vec(e.position());
        if (e.speed_mps() > 0) a.speed_mps = e.speed_mps();
        world_.actor_enter(t, a);
        break;
      }
      case v1::Stimulus::kActorMove: {
        std::vector<Vec2> path;
        for (const auto& p : s.actor_move().path()) path.push_back(to_vec(p));
        world_.actor_move(t, s.actor_move().actor(), path);
        break;
      }
      case v1::Stimulus::kActorLeave:
        world_.actor_leave(t, s.actor_leave().actor());
        break;
      case v1::Stimulus::kPortalSet:
        world_.set_portal_open(t, s.portal_set().portal_id(), s.portal_set().open());
        break;
      case v1::Stimulus::kImpact:
        world_.impact(t, s.impact().portal_id(), s.impact().magnitude_mg());
        break;
      case v1::Stimulus::kLinkSet:
        transport_.set_connected(s.link_set().device_id(), s.link_set().connected());
        world_.record(t, s.link_set().connected() ? "link.up" : "link.down",
                      s.link_set().device_id());
        break;
      case v1::Stimulus::kDeviceReboot:
        if (auto it = devices_.find(s.device_reboot().device_id()); it != devices_.end()) {
          world_.record(t, "device.reboot", it->first);
          it->second->reboot(t, clock_.now(), world_, up);
        }
        break;
      case v1::Stimulus::kDeviceClockSkew:
        if (auto it = devices_.find(s.device_clock_skew().device_id()); it != devices_.end()) {
          it->second->set_clock_skew(s.device_clock_skew().skew_ms());
        }
        break;
      case v1::Stimulus::kEnclosureSet:
        if (s.enclosure_set().bounce_ms() > 0) {
          world_.enclosure_bounce(t, s.enclosure_set().device_id(), s.enclosure_set().bounce_ms());
        } else {
          world_.set_enclosure(t, s.enclosure_set().device_id(), s.enclosure_set().open());
        }
        break;
      case v1::Stimulus::kUserButton:
        world_.press_user_button(t, s.user_button().device_id(), s.user_button().hold_ms());
        break;
      case v1::Stimulus::kBatterySet:
        world_.set_battery(t, s.battery_set().device_id(), s.battery_set().mv());
        break;
      case v1::Stimulus::kUsbConsole:
        world_.usb_console(t, s.usb_console().device_id(), s.usb_console().line());
        break;
      case v1::Stimulus::kTemperatureSet:
        world_.set_temperature(t, s.temperature_set().zone_id(), s.temperature_set().milli_c());
        break;
      case v1::Stimulus::kArmModeSet: {
        auto mode = codec::from_proto(s.arm_mode_set().mode());
        if (!mode) break;
        Status st =
            engine_->set_arm_mode(mode.value(), PrincipalId(s.arm_mode_set().principal_id()));
        world_.record(t, st ? "arm.set" : "arm.denied", s.arm_mode_set().principal_id(),
                      st ? "" : st.error().message);
        break;
      }
      case v1::Stimulus::kUserCommand:
        request(t, s.user_command());
        break;
      case v1::Stimulus::kHubRestart:
        world_.record(t, "hub.restart", "hub");
        engine_ = std::make_unique<HubEngine>(config_, journal_, clock_);
        break;
      case v1::Stimulus::kActuatorFault:
        if (auto it = devices_.find(s.actuator_fault().device_id()); it != devices_.end()) {
          it->second->set_fault(s.actuator_fault().fault());
        }
        break;
      case v1::Stimulus::KIND_NOT_SET:
        break;
    }
    send_uplinks(t, up);
  }

  void request(std::int64_t t, const v1::UserCommand& u) {
    Command c;
    c.id = CommandId(u.command_id());
    c.idempotency_key = u.command_id();
    c.principal = PrincipalId(u.principal_id());
    c.property = config_.property.id;
    auto target = codec::from_proto(u.target());
    auto args = codec::from_proto(u.args());
    if (!target || !args) {
      report_.failures.push_back("user_command " + u.command_id() + ": invalid target/args");
      return;
    }
    c.target = target.value();
    c.operation = u.operation();
    c.args = args.value();
    c.created_at = clock_.now();
    c.expires_at = clock_.now() + Millis{u.ttl_ms() > 0 ? u.ttl_ms() : 5'000};
    c.verification_window =
        Millis{u.verification_window_ms() > 0 ? u.verification_window_ms() : 5'000};
    if (u.has_expect()) {
      auto src = codec::from_proto(u.expect().source());
      if (src) c.expect = ExpectedObservation{src.value(), codec::from_proto(u.expect().payload())};
      c.verification = VerificationMode::kObservedState;
    } else {
      c.verification = VerificationMode::kAckOnly;
    }
    world_.record(t, "user.command", u.principal_id(), u.command_id() + " " + u.operation());
    engine_->request(c);
  }

  void fail(const std::string& name, const std::string& why) {
    report_.failures.push_back((name.empty() ? std::string("expectation") : name) + ": " + why);
  }

  void evaluate() {
    const PropertyState& state = engine_->state();
    const auto entries = read_all(journal_);
    for (const auto& e : sc_.expect()) {
      switch (e.kind_case()) {
        case v1::Expectation::kObservation: {
          const auto& x = e.observation();
          const Timestamp from = start_ + Millis{x.from_ms()};
          const Timestamp to = start_ + Millis{x.to_ms() > 0 ? x.to_ms() : sc_.duration_ms()};
          std::uint32_t n = 0;
          for (const auto& entry : entries) {
            const auto* obs = std::get_if<ObservationRecorded>(&entry.event);
            if (obs && obs->envelope.event_type == x.event_type() &&
                (x.device_id().empty() || obs->envelope.source.device.str() == x.device_id()) &&
                entry.received_at >= from && entry.received_at <= to) {
              ++n;
            }
          }
          const std::uint32_t min = x.has_min_count() ? x.min_count() : 1;
          if (n < min || (x.has_max_count() && n > x.max_count())) {
            fail(e.name(), x.event_type() + " from " + x.device_id() + " seen " +
                               std::to_string(n) + " times in window");
          }
          break;
        }
        case v1::Expectation::kCommand: {
          const auto& x = e.command();
          std::uint32_t n = 0;
          std::string statuses;
          // Denied requests live apart from the id namespace in the ledger.
          std::vector<const CommandRecord*> all;
          for (const auto& [id, rec] : state.commands.records()) all.push_back(&rec);
          for (const auto& [key, rec] : state.commands.denied()) all.push_back(&rec);
          for (const CommandRecord* r : all) {
            const CommandRecord& rec = *r;
            if (rec.command.operation != x.operation()) continue;
            if (!x.target_device_id().empty() &&
                rec.command.target.device.str() != x.target_device_id()) {
              continue;
            }
            statuses += std::string(to_string(rec.status)) + " ";
            bool ok = x.final_status_in().empty();
            for (int s : x.final_status_in()) ok |= status_matches(rec.status, s);
            if (ok) ++n;
          }
          const bool count_ok = x.has_count() ? n == x.count() : n >= 1;
          if (!count_ok) {
            fail(e.name(), x.operation() + " matching commands: " + std::to_string(n) +
                               " (statuses: " + (statuses.empty() ? "none" : statuses) + ")");
          }
          break;
        }
        case v1::Expectation::kDiagnostic: {
          const auto& x = e.diagnostic();
          const std::int64_t to = x.to_ms() > 0 ? x.to_ms() : sc_.duration_ms();
          std::uint32_t n = 0;
          for (const auto& d : diagnostics_) {
            if (d.kind == x.kind() && (x.device_id().empty() || d.device == x.device_id()) &&
                d.t_ms >= x.from_ms() && d.t_ms <= to) {
              ++n;
            }
          }
          const std::uint32_t min = x.has_min_count() ? x.min_count() : 1;
          if (n < min || (x.has_max_count() && n > x.max_count())) {
            fail(e.name(), "diagnostic " + x.kind() + " from " + x.device_id() + " seen " +
                               std::to_string(n) + " times in window");
          }
          break;
        }
        case v1::Expectation::kNoCommand:
          for (const auto& [id, rec] : state.commands.records()) {
            if (rec.command.operation == e.no_command().operation()) {
              fail(e.name(), "unexpected command " + id.str() + " (" +
                                 std::string(to_string(rec.status)) + ")");
            }
          }
          for (const auto& [key, rec] : state.commands.denied()) {
            if (rec.command.operation == e.no_command().operation()) {
              fail(e.name(), "unexpected command " + key.second.str() + " (denied)");
            }
          }
          break;
        case v1::Expectation::kFreshness: {
          auto ref = codec::from_proto(e.freshness().ref());
          if (!ref) break;
          const Freshness got = engine_->freshness_of(ref.value());
          if (got != to_core(e.freshness().state())) {
            fail(e.name(), "freshness of " + ref.value().device.str() + "/" +
                               ref.value().capability.str() + " is " +
                               std::string(freshness_name(got)));
          }
          break;
        }
        case v1::Expectation::kPhysicalEffects: {
          const auto n = world_.effect_count(e.physical_effects().device_id());
          if (n != e.physical_effects().count()) {
            fail(e.name(), e.physical_effects().device_id() + " physical effects " +
                               std::to_string(n) + ", expected " +
                               std::to_string(e.physical_effects().count()));
          }
          break;
        }
        case v1::Expectation::kCapabilityState: {
          auto ref = codec::from_proto(e.capability_state().ref());
          if (!ref) break;
          auto it = state.capabilities.find(ref.value());
          const auto want = codec::from_proto(e.capability_state().payload());
          if (it == state.capabilities.end() || it->second.value != want) {
            fail(e.name(), "belief for " + ref.value().device.str() + "/" +
                               ref.value().capability.str() + " differs from expected");
          }
          break;
        }
        case v1::Expectation::kPortal: {
          const Portal* p = world_.portal(e.portal().portal_id());
          if (!p) {
            fail(e.name(), "unknown portal " + e.portal().portal_id());
          } else if ((e.portal().has_open() && p->open != e.portal().open()) ||
                     (e.portal().has_locked() && p->locked != e.portal().locked())) {
            fail(e.name(), "portal " + p->id + " ground truth open=" + std::to_string(p->open) +
                               " locked=" + std::to_string(p->locked));
          }
          break;
        }
        case v1::Expectation::kArmMode: {
          auto want = codec::from_proto(e.arm_mode().mode());
          if (want && state.arm_mode != want.value()) fail(e.name(), "arm mode differs");
          break;
        }
        case v1::Expectation::KIND_NOT_SET:
          break;
      }
    }
  }

  // Always-on checks comparing belief with ground truth.
  void check_invariants() {
    const PropertyState& state = engine_->state();
    std::map<std::string, bool> actuators;
    for (const auto& d : site_.devices())
      actuators[d.id()] = d.has_speaker() || d.has_lock() || d.has_notifier();
    for (const auto& [id, rec] : state.commands.records()) {
      // No fabricated verification: a verified actuator command must have
      // actually happened in the world.
      const std::string dev = rec.command.target.device.str();
      if (rec.status == CommandStatus::kVerified && actuators[dev] &&
          !world_.effects().contains({dev, id.str()})) {
        fail("invariant", "command " + id.str() + " verified but no physical effect occurred");
      }
    }
    for (const auto& [key, count] : world_.effects()) {
      if (count > 1) {
        fail("invariant", "command " + key.second + " acted " + std::to_string(count) +
                              " times on " + key.first);
      }
    }
    if (!(replay({}, journal_) == state)) {
      fail("invariant", "journal replay does not reproduce engine state");
    }
  }

  void finish() {
    report_.passed = report_.failures.empty();
    std::ostringstream trace;
    trace << "{\"scenario\":\"" << json_escape(report_.name) << "\",\"seed\":" << report_.seed
          << ",\"passed\":" << (report_.passed ? "true" : "false");
    if (report_.skipped) trace << ",\"skipped\":\"" << json_escape(report_.skip_reason) << "\"";
    trace << ",\"bindings\":{";
    for (std::size_t i = 0; i < report_.bindings.size(); ++i) {
      trace << (i ? "," : "") << "\"" << json_escape(report_.bindings[i].first) << "\":\""
            << report_.bindings[i].second << "\"";
    }
    trace << "},\"failures\":[";
    for (std::size_t i = 0; i < report_.failures.size(); ++i) {
      trace << (i ? "," : "") << "\"" << json_escape(report_.failures[i]) << "\"";
    }
    trace << "],\"ground_truth\":[";
    bool first = true;
    for (const auto& t : world_.truth()) {
      trace << (first ? "" : ",") << "{\"t_ms\":" << t.t_ms << ",\"kind\":\"" << json_escape(t.kind)
            << "\",\"subject\":\"" << json_escape(t.subject) << "\",\"detail\":\""
            << json_escape(t.detail) << "\"}";
      first = false;
    }
    trace << "],\"journal\":[";
    std::uint64_t h = 1469598103934665603ull;
    first = true;
    for (const auto& entry : read_all(journal_)) {
      if (const auto* o = std::get_if<ObservationRecorded>(&entry.event)) {
        ++report_.observations[o->envelope.source.device.str() + " " + o->envelope.event_type];
      }
      const auto p = codec::to_proto(entry);
      std::string bytes;
      (void)p.SerializeToString(&bytes);
      for (char c : bytes) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
      }
      std::string json;
      (void)google::protobuf::util::MessageToJsonString(p, &json);
      trace << (first ? "" : ",") << json;
      first = false;
      ++report_.journal_entries;
    }
    trace << "]}";
    report_.journal_digest = h;
    report_.trace_json = trace.str();
  }

  const v1::Scenario& sc_;
  const v1::Site& site_;
  RunOptions options_;
  Timestamp start_;
  VirtualClock clock_;
  World world_;
  Transport transport_;
  EngineConfig config_;
  InMemoryJournal journal_;
  std::unique_ptr<HubEngine> engine_;
  std::map<std::string, std::unique_ptr<IDeviceBackend>> devices_;
  struct SentDiagnostic {
    std::int64_t t_ms;
    std::string device;
    std::string kind;
    std::int64_t value;
  };
  std::vector<SentDiagnostic> diagnostics_;
  ScenarioReport report_;
};

}  // namespace

ScenarioReport run_scenario(const v1::Scenario& scenario, const v1::Site& site,
                            RunOptions options) {
  return Run(scenario, site, options).execute();
}

Result<ScenarioReport> run_scenario_file(const std::string& scenario_path,
                                         const std::string& corpus_root, RunOptions options) {
  v1::Scenario sc;
  if (Status s = codec::load_yaml_file(scenario_path, sc); !s) return s.error();
  v1::Site site;
  if (Status s = codec::load_yaml_file(corpus_root + "/" + sc.site(), site); !s) return s.error();
  return run_scenario(sc, site, options);
}

Status write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path);
  if (!out) return Error{"io_error", path};
  out << text;
  return Status::Ok();
}

}  // namespace talos::sim
