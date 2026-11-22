#include "simulation/twin/devices.h"

#include <limits>

#include "firmware/portable/console_policy.h"

namespace talos::sim {

namespace {
constexpr std::int64_t kTickMs = 100;
}

PirOptics::PirOptics(Vec2 position, const v1::PirSpec& spec)
    : position_(position),
      spec_(spec),
      wedge_(position, spec.heading_deg(), spec.half_angle_deg(), spec.range_m()) {}

bool PirOptics::step(std::int64_t t_ms, const World& world, Rng& rng) {
  bool raw = false;
  for (const auto& [aid, actor] : world.actors()) {
    if (!actor.moved_this_tick || !wedge_.contains(actor.position) ||
        !line_of_sight(position_, actor.position, world.walls())) {
      continue;
    }
    if (actor.kind == ActorKind::kPet) {
      auto [it, inserted] = pet_visible_.try_emplace(aid, false);
      if (inserted) it->second = rng.chance(spec_.pet_sensitivity());
      if (!it->second) continue;
    }
    raw = true;
  }
  const double false_p = spec_.false_triggers_per_hour() * kTickMs / 3'600'000.0;
  if (!raw && !active_ && rng.chance(false_p)) raw = true;

  if (raw) {
    if (!active_ && !suppressed_) {
      if (rng.chance(spec_.miss_probability())) {
        suppressed_ = true;
      } else {
        active_ = true;
      }
    }
    hold_until_ = t_ms + spec_.hold_ms();
  } else {
    suppressed_ = false;
    if (active_ && t_ms >= hold_until_) active_ = false;
  }
  return active_;
}

VirtualDevice::VirtualDevice(const v1::SiteDevice& spec, PropertyId property, std::uint64_t seed)
    : id_(spec.id()),
      property_(std::move(property)),
      position_(to_vec(spec.position())),
      rng_(Rng::stream(seed, spec.id())),
      heartbeat_interval_ms_(spec.heartbeat_interval_ms()),
      battery_mv_(spec.battery_mv()) {
  for (const auto& s : spec.sensors()) {
    const auto slot = static_cast<std::uint8_t>(s.slot());
    switch (s.kind_case()) {
      case v1::SensorSpec::kPir:
        pirs_.push_back(Pir{slot, PirOptics(position_, s.pir())});
        break;
      case v1::SensorSpec::kContact:
        contacts_.push_back({slot, s.contact().portal_id(), {}});
        break;
      case v1::SensorSpec::kImpact:
        impacts_.push_back({slot, s.impact()});
        break;
      case v1::SensorSpec::kEnvironment:
        environments_.push_back({slot, s.environment(), 0});
        break;
      case v1::SensorSpec::kCamera:
        cameras_.push_back(Camera{slot, s.camera(),
                                  Wedge(position_, s.camera().heading_deg(),
                                        s.camera().half_angle_deg(), s.camera().range_m())});
        break;
      case v1::SensorSpec::KIND_NOT_SET:
        break;
    }
  }
  if (spec.has_speaker()) speaker_ = spec.speaker();
  if (spec.has_lock()) lock_ = spec.lock();
  if (spec.has_notifier()) notifier_ = spec.notifier();
  if (spec.has_enclosure()) enclosure_ = spec.enclosure();
  // TL-SN-002's board table (firmware/esp_idf/main/board_tl_sn_002.h) locks the console.
  console_lock_ = spec.board_id() == "sensor_node_v2";
}

ObservationEnvelope VirtualDevice::envelope(Timestamp now, std::optional<std::uint8_t> slot,
                                            const std::string& capability, std::string event_type,
                                            ObservationPayload payload) {
  ++sequence_;
  ObservationEnvelope e;
  e.property = property_;
  e.source = {DeviceId(id_), slot, CapabilityId(capability)};
  e.event_type = std::move(event_type);
  e.event_id = EventId(id_ + ":" + std::to_string(boot_epoch_) + ":" + std::to_string(sequence_));
  e.position = {boot_epoch_, sequence_};
  e.observed_at = now + Millis{skew_ms_};
  e.payload = std::move(payload);
  return e;
}

void VirtualDevice::report_initial_state(Timestamp now, const World& world,
                                         std::vector<Uplink>& out) {
  for (auto& c : contacts_) {
    if (const Portal* p = world.portal(c.portal)) {
      c.last = p->open;
      // Boot state report, not a transition: rules must not fire on it.
      emit(out,
           envelope(now, c.slot, "contact.open_state", "contact.state", ContactState{p->open}));
    }
  }
  if (enclosure_) {
    // Boot state report (b = 1 on the wire): rules don't fire on it.
    enclosure_last_ = world.enclosure_open(id_);
    emit(out, envelope(now, std::nullopt, "tamper.enclosure", "tamper.state",
                       TamperState{enclosure_last_}));
    if (console_lock_) {  // TL-SN-002: the battery door reports its own state (HD-049)
      door_last_ = world.battery_door_open(id_);
      emit(out, envelope(now, std::nullopt, "tamper.battery_door", "tamper.state",
                         TamperState{door_last_}));
    }
  }
  if (lock_) {
    if (const Portal* p = world.portal(lock_->portal_id())) {
      emit(out, envelope(now, std::nullopt, "access.lock", "lock.state", LockState{p->locked}));
    }
  }
  if (heartbeat_interval_ms_ > 0) {
    emit(out,
         envelope(now, std::nullopt, "device.health", "device.heartbeat", Heartbeat{battery_mv_}));
  }
}

void VirtualDevice::boot(std::int64_t t_ms, Timestamp now, const World& world,
                         std::vector<Uplink>& out) {
  start(t_ms, now, world, out);
  boot_ms_ = std::numeric_limits<std::int32_t>::min();  // installed earlier: already warm
}

void VirtualDevice::start(std::int64_t t_ms, Timestamp now, const World& world,
                          std::vector<Uplink>& out) {
  boot_ms_ = t_ms;
  next_heartbeat_ = t_ms + heartbeat_interval_ms_;
  for (auto& e : environments_) e.next_at = t_ms;
  report_initial_state(now, world, out);
}

void VirtualDevice::reboot(std::int64_t t_ms, Timestamp now, const World& world,
                           std::vector<Uplink>& out) {
  ++boot_epoch_;
  sequence_ = 0;
  pending_.clear();
  for (auto& p : pirs_) p.optics.reset();
  console_unreported_ = 0;  // RAM: lost in a reset, as on the node
  console_reported_ms_.reset();
  start(t_ms, now, world, out);
}

void VirtualDevice::sample(std::int64_t t_ms, Timestamp now, World& world,
                           const std::vector<std::pair<std::string, std::uint32_t>>& impacts,
                           std::vector<Uplink>& out) {
  // PIR: moving, visible actors inside the wedge.
  for (auto& pir : pirs_) {
    if (t_ms - boot_ms_ < pir.optics.spec().warmup_ms()) continue;
    const bool was = pir.optics.active();
    const bool now_active = pir.optics.step(t_ms, world, rng_);
    if (now_active != was) {
      emit(out,
           envelope(now, pir.slot, "motion.detect",
                    now_active ? "motion.detected" : "motion.cleared", MotionState{now_active}));
    }
  }

  for (auto& c : contacts_) {
    const Portal* p = world.portal(c.portal);
    if (!p || c.last == p->open) continue;
    c.last = p->open;
    emit(out, envelope(now, c.slot, "contact.open_state",
                       p->open ? "contact.opened" : "contact.closed", ContactState{p->open}));
  }

  if (enclosure_) {
    // A loop change that lasts a tick (100 ms) is past any debounce; a bounce counts only
    // if it outlasts the debounce, and then reads as an opening and a closing.
    const std::uint32_t debounce = enclosure_->debounce_ms() ? enclosure_->debounce_ms() : 50;
    for (const std::uint32_t ms : world.take_enclosure_bounces(id_)) {
      if (ms < debounce || enclosure_last_) continue;
      emit(out,
           envelope(now, std::nullopt, "tamper.enclosure", "tamper.opened", TamperState{true}));
      emit(out,
           envelope(now, std::nullopt, "tamper.enclosure", "tamper.closed", TamperState{false}));
    }
    if (const bool open = world.enclosure_open(id_); open != enclosure_last_) {
      enclosure_last_ = open;
      emit(out, envelope(now, std::nullopt, "tamper.enclosure",
                         open ? "tamper.opened" : "tamper.closed", TamperState{open}));
    }
    if (const bool door = world.battery_door_open(id_); console_lock_ && door != door_last_) {
      door_last_ = door;
      emit(out, envelope(now, std::nullopt, "tamper.battery_door",
                         door ? "tamper.opened" : "tamper.closed", TamperState{door}));
    }
  }

  // USB console (TL-SN-002, firmware/portable/console_policy.h): read-only while the
  // lid is closed (as last reported, i.e. debounced). A refusal is counted and sent as
  // a "console.locked" diagnostic: the first at once, later ones once per window.
  for (const std::string& line : world.take_usb_console(id_)) {
    const bool locked = console_lock_ && !enclosure_last_;
    if (locked && fw::console_line_access(line.c_str()) == fw::ConsoleAccess::kMutating) {
      world.record(t_ms, "console.refused", id_, line);
      ++console_unreported_;
    } else {
      world.record(t_ms, "console.accepted", id_, line);
    }
  }
  if (console_unreported_ > 0 &&
      (!console_reported_ms_ || t_ms - *console_reported_ms_ >= fw::kConsoleReportMs)) {
    out.push_back({id_, DeviceDiagnostic{"console.locked", console_unreported_}});
    console_unreported_ = 0;
    console_reported_ms_ = t_ms;
  }

  for (const auto& imp : impacts_) {
    for (const auto& [portal, mg] : impacts) {
      if (portal != imp.spec.portal_id()) continue;
      const auto effective = static_cast<std::uint32_t>(mg * imp.spec.coupling());
      if (effective >= imp.spec.threshold_mg()) {
        emit(out, envelope(now, imp.slot, "impact.detect", "impact.detected",
                           ImpactDetected{static_cast<std::int32_t>(effective), 12}));
      }
    }
  }

  for (auto& env : environments_) {
    if (t_ms < env.next_at) continue;
    env.next_at = t_ms + env.spec.report_interval_ms();
    const auto zone = world.zone_at(position_).value_or("");
    const auto noise = static_cast<std::int32_t>(rng_.between(
        -static_cast<std::int64_t>(env.spec.noise_milli_c()), env.spec.noise_milli_c()));
    emit(out, envelope(now, env.slot, "environment.temperature_humidity", "environment.reading",
                       TemperatureHumidity{
                           world.temperature(zone) + env.spec.bias_milli_c() + noise, 450}));
  }

  for (auto& cam : cameras_) {
    if (t_ms - cam.last_emit < cam.spec.min_interval_ms()) continue;
    for (const auto& [aid, actor] : world.actors()) {
      if (cam.wedge.contains(actor.position) &&
          line_of_sight(position_, actor.position, world.walls())) {
        cam.last_emit = t_ms;
        emit(out,
             envelope(now, cam.slot, "camera.detection", "camera.object_detected",
                      ObjectDetected{actor.kind == ActorKind::kPerson ? "person" : "animal",
                                     static_cast<std::uint16_t>(cam.spec.confidence_permille())}));
        break;
      }
    }
  }

  if (const auto mv = world.battery_mv(id_)) battery_mv_ = *mv;
  // USER button (TL-SN-002): a short press is a test report, i.e. a heartbeat now
  // (firmware: on release, after the debounce). A long press is a commissioning
  // request, which only the observability plane sees.
  for (const std::uint32_t hold_ms : world.take_user_presses(id_)) {
    if (hold_ms < kUserLongPressMs) {
      if (last_user_report_ms_ && t_ms - *last_user_report_ms_ < kUserCooldownMs) continue;
      last_user_report_ms_ = t_ms;
      emit(out, envelope(now, std::nullopt, "device.health", "device.heartbeat",
                         Heartbeat{battery_mv_}));
    }
  }

  if (heartbeat_interval_ms_ > 0 && t_ms >= next_heartbeat_) {
    next_heartbeat_ = t_ms + heartbeat_interval_ms_;
    emit(out,
         envelope(now, std::nullopt, "device.health", "device.heartbeat", Heartbeat{battery_mv_}));
  }

  // Actuator completions.
  std::vector<PendingAction> still;
  for (const auto& a : pending_) {
    if (a.due_ms <= t_ms) {
      complete(t_ms, now, a, world, out);
    } else {
      still.push_back(a);
    }
  }
  pending_.swap(still);
}

void VirtualDevice::receive(std::int64_t t_ms, const Command& command, World& world) {
  if (fault_ == v1::ACTUATOR_FAULT_NO_RESPONSE) {
    world.record(t_ms, "device.ignored", id_, command.id.str());
    return;
  }
  if (!executed_.insert(command.id.str()).second) {
    world.record(t_ms, "device.duplicate_command", id_, command.id.str());
    return;  // device-side idempotency: a redelivered command acts once
  }
  std::int64_t latency = 0;
  if (std::holds_alternative<AudioSpeak>(command.args) && speaker_) {
    latency = speaker_->latency_ms();
  } else if (std::holds_alternative<SetLocked>(command.args) && lock_) {
    latency = lock_->travel_ms();
  } else if (std::holds_alternative<Notify>(command.args) && notifier_) {
    latency = notifier_->latency_ms();
  }
  pending_.push_back({t_ms + latency, command});
}

void VirtualDevice::complete(std::int64_t t_ms, Timestamp now, const PendingAction& action,
                             World& world, std::vector<Uplink>& out) {
  const Command& cmd = action.command;
  auto ack = [&](bool accepted, std::string detail) {
    out.push_back({id_, CommandAck{cmd.id, accepted, std::move(detail)}});
  };
  if (fault_ == v1::ACTUATOR_FAULT_SILENT_ACK) {
    ack(true, "ok");
    return;
  }
  if (const auto* speak = std::get_if<AudioSpeak>(&cmd.args); speak && speaker_) {
    world.record_effect(t_ms, id_, cmd.id.str(), "played \"" + speak->text + "\"");
    emit(out,
         envelope(now, std::nullopt, "audio.speak", "audio.playback_started", AudioPlayback{true}));
    ack(true, "playing");
    return;
  }
  if (const auto* set = std::get_if<SetLocked>(&cmd.args); set && lock_) {
    ack(true, "moving");
    bool moved = false;
    if (fault_ != v1::ACTUATOR_FAULT_JAM) {
      moved = world.set_portal_locked(t_ms, lock_->portal_id(), set->locked);
    } else {
      world.record(t_ms, "lock.jammed", id_, cmd.id.str());
    }
    if (moved) world.record_effect(t_ms, id_, cmd.id.str(), set->locked ? "locked" : "unlocked");
    const Portal* p = world.portal(lock_->portal_id());
    emit(out, envelope(now, std::nullopt, "access.lock", "lock.state",
                       LockState{p ? p->locked : false}));
    return;
  }
  if (const auto* note = std::get_if<Notify>(&cmd.args); note && notifier_) {
    world.record_effect(t_ms, id_, cmd.id.str(), "notified " + note->recipient_role);
    ack(true, "delivered");
    return;
  }
  ack(false, "unsupported command");
}

}  // namespace talos::sim
