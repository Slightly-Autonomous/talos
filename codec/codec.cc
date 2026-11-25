#include "codec/codec.h"

#include <limits>
#include <string>

namespace talos::codec {

namespace v1 = ::talos::v1;

namespace {

Error range_error(const std::string& field) { return {"out_of_range", field + " out of range"}; }

template <typename To, typename From>
bool fits(From v) {
  return v <= static_cast<From>(std::numeric_limits<To>::max());
}

// ---- CapabilityRef / SourcePosition -------------------------------------

Result<CapabilityRef> ref_from(const v1::CapabilityRef& p) {
  CapabilityRef r;
  r.device = DeviceId(p.device_id());
  if (p.has_slot()) {
    if (!fits<SlotIndex>(p.slot())) return range_error("slot");
    r.slot = static_cast<SlotIndex>(p.slot());
  }
  r.capability = CapabilityId(p.capability_id());
  return r;
}

v1::CapabilityRef ref_to(const CapabilityRef& r) {
  v1::CapabilityRef p;
  p.set_device_id(r.device.str());
  if (r.slot) p.set_slot(*r.slot);
  p.set_capability_id(r.capability.str());
  return p;
}

// ---- Payloads ---------------------------------------------------------------

ObservationPayload payload_from(const v1::ObservationPayload& p) {
  switch (p.kind_case()) {
    case v1::ObservationPayload::kContact:
      return ContactState{p.contact().open()};
    case v1::ObservationPayload::kMotion:
      return MotionState{p.motion().detected()};
    case v1::ObservationPayload::kImpact:
      return ImpactDetected{p.impact().peak_mg(), p.impact().duration_ms()};
    case v1::ObservationPayload::kTemperatureHumidity:
      return TemperatureHumidity{p.temperature_humidity().temperature_milli_c(),
                                 p.temperature_humidity().humidity_permille()};
    case v1::ObservationPayload::kLock:
      return LockState{p.lock().locked()};
    case v1::ObservationPayload::kAudioPlayback:
      return AudioPlayback{p.audio_playback().playing()};
    case v1::ObservationPayload::kObjectDetected: {
      const auto conf = p.object_detected().confidence_permille();
      return ObjectDetected{p.object_detected().label(),
                            static_cast<std::uint16_t>(conf > 1000 ? 1000 : conf)};
    }
    case v1::ObservationPayload::kHeartbeat:
      return Heartbeat{p.heartbeat().battery_mv()};
    case v1::ObservationPayload::kTamper:
      return TamperState{p.tamper().open()};
    case v1::ObservationPayload::KIND_NOT_SET:
      break;
  }
  return std::monostate{};
}

struct PayloadTo {
  v1::ObservationPayload& p;
  void operator()(std::monostate) const {}
  void operator()(const ContactState& v) const { p.mutable_contact()->set_open(v.open); }
  void operator()(const MotionState& v) const { p.mutable_motion()->set_detected(v.detected); }
  void operator()(const ImpactDetected& v) const {
    p.mutable_impact()->set_peak_mg(v.peak_mg);
    p.mutable_impact()->set_duration_ms(v.duration_ms);
  }
  void operator()(const TemperatureHumidity& v) const {
    p.mutable_temperature_humidity()->set_temperature_milli_c(v.temperature_milli_c);
    p.mutable_temperature_humidity()->set_humidity_permille(v.humidity_permille);
  }
  void operator()(const LockState& v) const { p.mutable_lock()->set_locked(v.locked); }
  void operator()(const AudioPlayback& v) const {
    p.mutable_audio_playback()->set_playing(v.playing);
  }
  void operator()(const ObjectDetected& v) const {
    p.mutable_object_detected()->set_label(v.label);
    p.mutable_object_detected()->set_confidence_permille(v.confidence_permille);
  }
  void operator()(const Heartbeat& v) const { p.mutable_heartbeat()->set_battery_mv(v.battery_mv); }
  void operator()(const TamperState& v) const { p.mutable_tamper()->set_open(v.open); }
};

v1::ObservationPayload payload_to(const ObservationPayload& v) {
  v1::ObservationPayload p;
  std::visit(PayloadTo{p}, v);
  return p;
}

Result<CommandArgs> args_from(const v1::CommandArgs& p) {
  switch (p.kind_case()) {
    case v1::CommandArgs::kAudioSpeak: {
      if (!fits<std::uint8_t>(p.audio_speak().volume_percent())) {
        return range_error("volume_percent");
      }
      return CommandArgs{AudioSpeak{p.audio_speak().text(),
                                    static_cast<std::uint8_t>(p.audio_speak().volume_percent())}};
    }
    case v1::CommandArgs::kSetLocked:
      return CommandArgs{SetLocked{p.set_locked().locked()}};
    case v1::CommandArgs::kNotify:
      return CommandArgs{Notify{p.notify().recipient_role(), p.notify().text()}};
    case v1::CommandArgs::kSetMaintenance:
      return CommandArgs{SetMaintenance{p.set_maintenance().duration_ms()}};
    case v1::CommandArgs::KIND_NOT_SET:
      break;
  }
  return CommandArgs{std::monostate{}};
}

struct ArgsTo {
  v1::CommandArgs& p;
  void operator()(std::monostate) const {}
  void operator()(const AudioSpeak& a) const {
    p.mutable_audio_speak()->set_text(a.text);
    p.mutable_audio_speak()->set_volume_percent(a.volume_percent);
  }
  void operator()(const SetLocked& a) const { p.mutable_set_locked()->set_locked(a.locked); }
  void operator()(const Notify& a) const {
    p.mutable_notify()->set_recipient_role(a.recipient_role);
    p.mutable_notify()->set_text(a.text);
  }
  void operator()(const SetMaintenance& a) const {
    p.mutable_set_maintenance()->set_duration_ms(a.duration_ms);
  }
};

// ---- Enums --------------------------------------------------------------------

Result<CommandStatus> status_from(v1::CommandStatus s) {
  switch (s) {
    case v1::COMMAND_STATUS_REQUESTED:
      return CommandStatus::kRequested;
    case v1::COMMAND_STATUS_DENIED:
      return CommandStatus::kDenied;
    case v1::COMMAND_STATUS_ACCEPTED:
      return CommandStatus::kAccepted;
    case v1::COMMAND_STATUS_EXPIRED:
      return CommandStatus::kExpired;
    case v1::COMMAND_STATUS_CANCELED:
      return CommandStatus::kCanceled;
    case v1::COMMAND_STATUS_DISPATCHED:
      return CommandStatus::kDispatched;
    case v1::COMMAND_STATUS_DEVICE_ACKNOWLEDGED:
      return CommandStatus::kDeviceAcknowledged;
    case v1::COMMAND_STATUS_REJECTED:
      return CommandStatus::kRejected;
    case v1::COMMAND_STATUS_COMPLETED_UNVERIFIED:
      return CommandStatus::kCompletedUnverified;
    case v1::COMMAND_STATUS_VERIFIED:
      return CommandStatus::kVerified;
    case v1::COMMAND_STATUS_UNKNOWN_RESULT:
      return CommandStatus::kUnknownResult;
    default:
      break;
  }
  return Error{"invalid_enum", "command status " + std::to_string(static_cast<int>(s))};
}

v1::CommandStatus status_to(CommandStatus s) {
  switch (s) {
    case CommandStatus::kRequested:
      return v1::COMMAND_STATUS_REQUESTED;
    case CommandStatus::kDenied:
      return v1::COMMAND_STATUS_DENIED;
    case CommandStatus::kAccepted:
      return v1::COMMAND_STATUS_ACCEPTED;
    case CommandStatus::kExpired:
      return v1::COMMAND_STATUS_EXPIRED;
    case CommandStatus::kCanceled:
      return v1::COMMAND_STATUS_CANCELED;
    case CommandStatus::kDispatched:
      return v1::COMMAND_STATUS_DISPATCHED;
    case CommandStatus::kDeviceAcknowledged:
      return v1::COMMAND_STATUS_DEVICE_ACKNOWLEDGED;
    case CommandStatus::kRejected:
      return v1::COMMAND_STATUS_REJECTED;
    case CommandStatus::kCompletedUnverified:
      return v1::COMMAND_STATUS_COMPLETED_UNVERIFIED;
    case CommandStatus::kVerified:
      return v1::COMMAND_STATUS_VERIFIED;
    case CommandStatus::kUnknownResult:
      return v1::COMMAND_STATUS_UNKNOWN_RESULT;
  }
  return v1::COMMAND_STATUS_UNSPECIFIED;
}

Result<SourceOrder> order_from(v1::SourceOrder o) {
  switch (o) {
    case v1::SOURCE_ORDER_IN_ORDER:
      return SourceOrder::kInOrder;
    case v1::SOURCE_ORDER_GAP:
      return SourceOrder::kGap;
    case v1::SOURCE_ORDER_LATE:
      return SourceOrder::kLate;
    case v1::SOURCE_ORDER_NEW_BOOT:
      return SourceOrder::kNewBoot;
    default:
      break;
  }
  return Error{"invalid_enum", "source order"};
}

v1::SourceOrder order_to(SourceOrder o) {
  switch (o) {
    case SourceOrder::kInOrder:
      return v1::SOURCE_ORDER_IN_ORDER;
    case SourceOrder::kGap:
      return v1::SOURCE_ORDER_GAP;
    case SourceOrder::kLate:
      return v1::SOURCE_ORDER_LATE;
    case SourceOrder::kNewBoot:
      return v1::SOURCE_ORDER_NEW_BOOT;
  }
  return v1::SOURCE_ORDER_UNSPECIFIED;
}

Result<ArmMode> arm_from(v1::ArmMode m) {
  switch (m) {
    case v1::ARM_MODE_DISARMED:
      return ArmMode::kDisarmed;
    case v1::ARM_MODE_ARMED_HOME:
      return ArmMode::kArmedHome;
    case v1::ARM_MODE_ARMED_AWAY:
      return ArmMode::kArmedAway;
    default:
      break;
  }
  return Error{"invalid_enum", "arm mode"};
}

v1::ArmMode arm_to(ArmMode m) {
  switch (m) {
    case ArmMode::kDisarmed:
      return v1::ARM_MODE_DISARMED;
    case ArmMode::kArmedHome:
      return v1::ARM_MODE_ARMED_HOME;
    case ArmMode::kArmedAway:
      return v1::ARM_MODE_ARMED_AWAY;
  }
  return v1::ARM_MODE_UNSPECIFIED;
}

Result<PowerSource> source_from(v1::PowerSource s) {
  switch (s) {
    case v1::POWER_SOURCE_BATTERY_ONLY:
      return PowerSource::kBatteryOnly;
    case v1::POWER_SOURCE_USB:
      return PowerSource::kUsb;
    case v1::POWER_SOURCE_SOLAR_BATTERY:
      return PowerSource::kSolarBattery;
    case v1::POWER_SOURCE_POE:
      return PowerSource::kPoe;
    default:
      break;
  }
  return Error{"invalid_enum", "power source"};
}

}  // namespace

// ---- Observation ----------------------------------------------------------------

Result<ObservationEnvelope> from_proto(const v1::ObservationEnvelope& p) {
  ObservationEnvelope e;
  e.schema_version = p.schema_version();
  e.property = PropertyId(p.property_id());
  auto ref = ref_from(p.source());
  if (!ref) return ref.error();
  e.source = ref.value();
  if (p.has_module_id()) e.module = ModuleId(p.module_id());
  e.event_type = p.event_type();
  e.event_id = EventId(p.event_id());
  e.position = {p.position().boot_epoch(), p.position().sequence()};
  if (p.has_observed_at_unix_ms()) e.observed_at = Timestamp{p.observed_at_unix_ms()};
  e.payload = payload_from(p.payload());
  if (p.has_causation_id()) e.causation = EventId(p.causation_id());
  return e;
}

v1::ObservationEnvelope to_proto(const ObservationEnvelope& e) {
  v1::ObservationEnvelope p;
  p.set_schema_version(e.schema_version);
  p.set_property_id(e.property.str());
  *p.mutable_source() = ref_to(e.source);
  if (e.module) p.set_module_id(e.module->str());
  p.set_event_type(e.event_type);
  p.set_event_id(e.event_id.str());
  p.mutable_position()->set_boot_epoch(e.position.boot_epoch);
  p.mutable_position()->set_sequence(e.position.sequence);
  if (e.observed_at) p.set_observed_at_unix_ms(e.observed_at->unix_ms);
  *p.mutable_payload() = payload_to(e.payload);
  if (e.causation) p.set_causation_id(e.causation->str());
  return p;
}

// ---- Command ----------------------------------------------------------------------

Result<Command> from_proto(const v1::Command& p) {
  Command c;
  c.id = CommandId(p.id());
  c.idempotency_key = p.idempotency_key();
  c.principal = PrincipalId(p.principal_id());
  c.property = PropertyId(p.property_id());
  auto target = ref_from(p.target());
  if (!target) return target.error();
  c.target = target.value();
  c.operation = p.operation();
  auto args = args_from(p.args());
  if (!args) return args.error();
  c.args = args.value();
  c.created_at = Timestamp{p.created_at_unix_ms()};
  c.expires_at = Timestamp{p.expires_at_unix_ms()};
  switch (p.verification()) {
    case v1::VERIFICATION_MODE_ACK_ONLY:
      c.verification = VerificationMode::kAckOnly;
      break;
    case v1::VERIFICATION_MODE_OBSERVED_STATE:
      c.verification = VerificationMode::kObservedState;
      break;
    default:
      return Error{"invalid_enum", "verification mode"};
  }
  c.verification_window = Millis{p.verification_window_ms()};
  if (p.has_expect()) {
    auto src = ref_from(p.expect().source());
    if (!src) return src.error();
    c.expect = ExpectedObservation{src.value(), payload_from(p.expect().payload())};
  }
  if (p.has_causation_id()) c.causation = EventId(p.causation_id());
  return c;
}

v1::Command to_proto(const Command& c) {
  v1::Command p;
  p.set_id(c.id.str());
  p.set_idempotency_key(c.idempotency_key);
  p.set_principal_id(c.principal.str());
  p.set_property_id(c.property.str());
  *p.mutable_target() = ref_to(c.target);
  p.set_operation(c.operation);
  std::visit(ArgsTo{*p.mutable_args()}, c.args);
  p.set_created_at_unix_ms(c.created_at.unix_ms);
  p.set_expires_at_unix_ms(c.expires_at.unix_ms);
  p.set_verification(c.verification == VerificationMode::kAckOnly
                         ? v1::VERIFICATION_MODE_ACK_ONLY
                         : v1::VERIFICATION_MODE_OBSERVED_STATE);
  p.set_verification_window_ms(c.verification_window.count);
  if (c.expect) {
    *p.mutable_expect()->mutable_source() = ref_to(c.expect->source);
    *p.mutable_expect()->mutable_payload() = payload_to(c.expect->payload);
  }
  if (c.causation) p.set_causation_id(c.causation->str());
  return p;
}

Result<CommandTransition> from_proto(const v1::CommandTransition& p) {
  CommandTransition t;
  t.command = CommandId(p.command_id());
  auto to = status_from(p.to());
  if (!to) return to.error();
  t.to = to.value();
  t.at = Timestamp{p.at_unix_ms()};
  t.reason = p.reason();
  t.late = p.late();
  if (p.has_created()) {
    auto c = from_proto(p.created());
    if (!c) return c.error();
    t.created = c.value();
  }
  if (p.has_evidence_event_id()) t.evidence = EventId(p.evidence_event_id());
  return t;
}

v1::CommandTransition to_proto(const CommandTransition& t) {
  v1::CommandTransition p;
  p.set_command_id(t.command.str());
  p.set_to(status_to(t.to));
  p.set_at_unix_ms(t.at.unix_ms);
  p.set_reason(t.reason);
  p.set_late(t.late);
  if (t.created) *p.mutable_created() = to_proto(*t.created);
  if (t.evidence) p.set_evidence_event_id(t.evidence->str());
  return p;
}

// ---- Journal ------------------------------------------------------------------------

Result<JournalEntry> from_proto(const v1::JournalEntry& p) {
  JournalEntry e;
  e.journal_seq = p.journal_seq();
  e.received_at = Timestamp{p.received_at_unix_ms()};
  e.clock_regressed = p.clock_regressed();
  switch (p.event_case()) {
    case v1::JournalEntry::kObservation: {
      auto env = from_proto(p.observation().envelope());
      if (!env) return env.error();
      auto order = order_from(p.observation().order());
      if (!order) return order.error();
      e.event = ObservationRecorded{env.value(), order.value()};
      break;
    }
    case v1::JournalEntry::kCommandTransition: {
      auto t = from_proto(p.command_transition());
      if (!t) return t.error();
      e.event = t.value();
      break;
    }
    case v1::JournalEntry::kArmModeChanged: {
      auto mode = arm_from(p.arm_mode_changed().mode());
      if (!mode) return mode.error();
      e.event = ArmModeChanged{mode.value(), PrincipalId(p.arm_mode_changed().principal_id())};
      break;
    }
    case v1::JournalEntry::kConfigRevisionApplied:
      e.event = ConfigRevisionApplied{p.config_revision_applied().revision(),
                                      p.config_revision_applied().sha256(),
                                      PrincipalId(p.config_revision_applied().principal_id())};
      break;
    case v1::JournalEntry::kSourceReset:
      if (p.source_reset().device_id().empty()) {
        return Error{"invalid_journal_entry", "source_reset without device"};
      }
      e.event = SourceReset{DeviceId(p.source_reset().device_id()),
                            PrincipalId(p.source_reset().principal_id())};
      break;
    case v1::JournalEntry::kMaintenanceChanged: {
      const auto& m = p.maintenance_changed();
      if (m.device_id().empty())
        return Error{"invalid_journal_entry", "maintenance without device"};
      MaintenanceChanged out{DeviceId(m.device_id()), std::nullopt, PrincipalId(m.principal_id()),
                             m.reason(), std::nullopt};
      if (m.has_until_unix_ms()) out.until = Timestamp{m.until_unix_ms()};
      if (m.has_command_id()) out.command = CommandId(m.command_id());
      e.event = std::move(out);
      break;
    }
    case v1::JournalEntry::kAlarmSuppressed: {
      const auto& a = p.alarm_suppressed();
      e.event = AlarmSuppressed{
          DeviceId(a.device_id()), RuleId(a.rule_id()), EventId(a.causation_event_id()),
          Timestamp{a.maintenance_until_unix_ms()}, PrincipalId(a.maintenance_principal_id())};
      break;
    }
    case v1::JournalEntry::EVENT_NOT_SET:
      return Error{"invalid_journal_entry", "no event"};
  }
  return e;
}

v1::JournalEntry to_proto(const JournalEntry& e) {
  v1::JournalEntry p;
  p.set_journal_seq(e.journal_seq);
  p.set_received_at_unix_ms(e.received_at.unix_ms);
  p.set_clock_regressed(e.clock_regressed);
  if (const auto* obs = std::get_if<ObservationRecorded>(&e.event)) {
    *p.mutable_observation()->mutable_envelope() = to_proto(obs->envelope);
    p.mutable_observation()->set_order(order_to(obs->order));
  } else if (const auto* t = std::get_if<CommandTransition>(&e.event)) {
    *p.mutable_command_transition() = to_proto(*t);
  } else if (const auto* c = std::get_if<ConfigRevisionApplied>(&e.event)) {
    auto* out = p.mutable_config_revision_applied();
    out->set_revision(c->revision);
    out->set_sha256(c->sha256);
    out->set_principal_id(c->principal.str());
  } else if (const auto* r = std::get_if<SourceReset>(&e.event)) {
    p.mutable_source_reset()->set_device_id(r->device.str());
    p.mutable_source_reset()->set_principal_id(r->principal.str());
  } else if (const auto* m = std::get_if<MaintenanceChanged>(&e.event)) {
    auto* out = p.mutable_maintenance_changed();
    out->set_device_id(m->device.str());
    if (m->until) out->set_until_unix_ms(m->until->unix_ms);
    out->set_principal_id(m->principal.str());
    out->set_reason(m->reason);
    if (m->command) out->set_command_id(m->command->str());
  } else if (const auto* a = std::get_if<AlarmSuppressed>(&e.event)) {
    auto* out = p.mutable_alarm_suppressed();
    out->set_device_id(a->device.str());
    out->set_rule_id(a->rule.str());
    out->set_causation_event_id(a->causation.str());
    out->set_maintenance_until_unix_ms(a->maintenance_until.unix_ms);
    out->set_maintenance_principal_id(a->maintenance_by.str());
  } else {
    const auto& m = std::get<ArmModeChanged>(e.event);
    p.mutable_arm_mode_changed()->set_mode(arm_to(m.mode));
    p.mutable_arm_mode_changed()->set_principal_id(m.principal.str());
  }
  return p;
}

// ---- Configuration ------------------------------------------------------------------

Result<Property> from_proto(const v1::Property& p) {
  Property out;
  out.id = PropertyId(p.id());
  out.timezone = p.timezone();
  for (const auto& z : p.zones()) {
    out.zones.push_back(
        {ZoneId(z.id()), z.name(),
         z.kind() == v1::ZONE_KIND_OUTDOOR ? ZoneKind::kOutdoor : ZoneKind::kIndoor});
  }
  for (const auto& a : p.assets()) {
    Asset asset;
    asset.id = AssetId(a.id());
    asset.type = a.type();
    if (a.has_zone_id()) asset.zone = ZoneId(a.zone_id());
    for (const auto& att : a.attached()) {
      auto ref = ref_from(att);
      if (!ref) return ref.error();
      asset.attached.push_back(ref.value());
    }
    out.assets.push_back(std::move(asset));
  }
  for (const auto& d : p.devices()) {
    Device dev;
    dev.id = DeviceId(d.id());
    dev.property = PropertyId(d.property_id());
    if (d.has_board_id()) dev.board = BoardId(d.board_id());
    if (static_cast<std::size_t>(d.slot_modules_size()) > kMaxSlots) {
      return range_error("slot_modules");
    }
    for (int i = 0; i < d.slot_modules_size(); ++i) {
      if (!d.slot_modules(i).empty()) {
        dev.slots[static_cast<std::size_t>(i)] = ModuleId(d.slot_modules(i));
      }
    }
    if (d.has_zone_id()) dev.zone = ZoneId(d.zone_id());
    if (d.has_board_revision()) dev.board_revision = d.board_revision();
    for (const auto& c : d.capabilities()) {
      if (c.empty()) return Error{"invalid_property", "device " + d.id() + ": empty capability"};
      dev.capabilities.emplace_back(c);
    }
    out.devices.push_back(std::move(dev));
  }
  return out;
}

Result<BoardManifest> from_proto(const v1::BoardManifest& p) {
  BoardManifest b;
  b.id = BoardId(p.id());
  b.revision = p.revision();
  b.provisional = p.provisional();
  for (const auto& s : p.slots()) {
    if (!fits<SlotIndex>(s.index())) return range_error("slot index");
    SlotSpec slot;
    slot.index = static_cast<SlotIndex>(s.index());
    slot.wake = s.wake();
    if (s.has_i2c_branch()) {
      if (!fits<std::uint8_t>(s.i2c_branch())) return range_error("i2c_branch");
      slot.i2c_branch = static_cast<std::uint8_t>(s.i2c_branch());
    }
    slot.spi = s.spi();
    slot.uart = s.uart();
    slot.analog = s.analog();
    slot.rails.assign(s.rails().begin(), s.rails().end());
    if (!s.built_in_module().empty()) slot.built_in_module = ModuleId(s.built_in_module());
    b.slots.push_back(std::move(slot));
  }
  for (const auto& r : p.rails()) {
    b.rails.push_back({r.name(), r.voltage_mv(), r.max_ma(), r.per_slot_max_ma()});
  }
  for (const auto& ps : p.power_sources()) {
    auto src = source_from(ps.source());
    if (!src) return src.error();
    b.power_sources.push_back({src.value(), ps.sustained_mw(), ps.peak_mw(), ps.sustained_uw()});
  }
  b.connector_standard = p.connector_standard();
  b.uart_channels = p.uart_channels();
  for (auto addr : p.reserved_i2c_addresses()) {
    if (addr > 0x7F) return range_error("reserved i2c address");
    b.reserved_i2c_addresses.push_back(static_cast<std::uint8_t>(addr));
  }
  b.board_family = p.board_family();
  b.transport = p.transport();
  for (const auto& e : p.expanders()) {
    if (e.i2c_address() > 0x7F) return range_error("expander i2c address");
    ExpanderSpec x{
        e.name(), e.part(), static_cast<std::uint8_t>(e.i2c_address()), e.interrupt_net(), {}};
    for (const auto& pin : e.pins()) {
      if (pin.direction() != "input" && pin.direction() != "output" &&
          pin.direction() != "unused") {
        return Error{"invalid_manifest", "expander " + e.name() + " " + pin.pin() +
                                             ": direction must be "
                                             "input, output or unused"};
      }
      x.pins.push_back({pin.pin(), pin.net(), pin.direction(), pin.active_low()});
    }
    b.expanders.push_back(std::move(x));
  }
  for (const auto& w : p.wake_lines())
    b.wake_lines.push_back({w.net(), w.mcu_pin(), w.active_high()});
  for (const auto& c : p.board_capabilities()) {
    if (c.empty()) return Error{"invalid_manifest", "empty board capability"};
    b.board_capabilities.emplace_back(c);
  }
  for (const auto& l : p.module_slot_limits()) {
    if (l.module_id().empty()) return Error{"invalid_manifest", "module_slot_limits: empty module"};
    ModuleSlotLimit lim{ModuleId(l.module_id()), {}, l.reason()};
    for (auto s : l.allowed_slots()) {
      if (!fits<SlotIndex>(s)) return range_error("allowed slot");
      lim.allowed_slots.push_back(static_cast<SlotIndex>(s));
    }
    b.module_slot_limits.push_back(std::move(lim));
  }
  for (const auto& i : p.indicators()) {
    b.indicators.push_back({i.name(), i.net(), i.driver(), i.active_low(), i.color()});
  }
  if (p.has_battery()) {
    const auto& bat = p.battery();
    if (bat.cells_in_series() == 0) return Error{"invalid_manifest", "battery: no cells"};
    if (bat.critical_mv() > bat.low_mv() || bat.low_mv() > bat.fresh_mv()) {
      return Error{"invalid_manifest", "battery: want critical_mv <= low_mv <= fresh_mv"};
    }
    b.battery =
        BatterySpec{bat.chemistry(), bat.form(),   bat.cells_in_series(), bat.rechargeable(),
                    bat.fresh_mv(),  bat.low_mv(), bat.critical_mv()};
  }
  return b;
}

Result<ModuleManifest> from_proto(const v1::ModuleManifest& p) {
  ModuleManifest m;
  m.id = ModuleId(p.id());
  m.revision = p.revision();
  m.provisional = p.provisional();
  m.needs_wake = p.needs_wake();
  for (auto addr : p.i2c_addresses()) {
    if (addr > 0x7F) return range_error("i2c address");
    m.i2c_addresses.push_back(static_cast<std::uint8_t>(addr));
  }
  m.needs_spi = p.needs_spi();
  m.needs_uart = p.needs_uart();
  m.needs_analog = p.needs_analog();
  for (const auto& l : p.loads()) m.loads.push_back({l.rail(), l.active_ma()});
  m.average_mw = p.average_mw();
  m.peak_mw = p.peak_mw();
  m.average_uw = p.average_uw();
  for (const auto& c : p.capabilities()) m.capabilities.emplace_back(c);
  m.connector_standard = p.connector_standard();
  m.test_fixture = p.test_fixture();
  return m;
}

// ---- Public helpers -------------------------------------------------------------------

Result<CapabilityRef> from_proto(const v1::CapabilityRef& p) { return ref_from(p); }
ObservationPayload from_proto(const v1::ObservationPayload& p) { return payload_from(p); }
Result<CommandArgs> from_proto(const v1::CommandArgs& p) { return args_from(p); }
Result<ArmMode> from_proto(v1::ArmMode m) { return arm_from(m); }

Result<Rule> from_proto(const v1::Rule& p) {
  Rule r;
  r.id = RuleId(p.id());
  r.revision = p.revision();
  r.property = PropertyId(p.property_id());
  r.principal = PrincipalId(p.principal_id());
  r.enabled = p.enabled();
  r.trigger.event_type = p.trigger_event_type();
  if (p.has_trigger_zone_id()) r.trigger.zone = ZoneId(p.trigger_zone_id());
  for (int m : p.armed_modes()) {
    auto mode = arm_from(static_cast<v1::ArmMode>(m));
    if (!mode) return mode.error();
    r.conditions.armed_modes.push_back(mode.value());
  }
  if (p.has_schedule()) {
    if (p.schedule().start_minute() >= 1440 || p.schedule().end_minute() >= 1440) {
      return range_error("schedule minute");
    }
    r.conditions.schedule = TimeWindow{static_cast<std::uint16_t>(p.schedule().start_minute()),
                                       static_cast<std::uint16_t>(p.schedule().end_minute())};
  }
  for (const auto& a : p.actions()) {
    RuleAction action;
    action.operation = a.operation();
    auto target = ref_from(a.target());
    if (!target) return target.error();
    action.target = target.value();
    auto args = args_from(a.args());
    if (!args) return args.error();
    action.args = args.value();
    if (a.ttl_ms() > 0) action.ttl = Millis{a.ttl_ms()};
    if (a.verification_window_ms() > 0)
      action.verification_window = Millis{a.verification_window_ms()};
    switch (a.verification()) {
      case v1::VERIFICATION_MODE_ACK_ONLY:
        action.verification = VerificationMode::kAckOnly;
        break;
      case v1::VERIFICATION_MODE_OBSERVED_STATE:
      case v1::VERIFICATION_MODE_UNSPECIFIED:
        action.verification = VerificationMode::kObservedState;
        break;
      default:
        return Error{"invalid_enum", "verification mode"};
    }
    if (a.has_expect()) {
      auto src = ref_from(a.expect().source());
      if (!src) return src.error();
      action.expect = ExpectedObservation{src.value(), payload_from(a.expect().payload())};
    }
    r.actions.push_back(std::move(action));
  }
  return r;
}

}  // namespace talos::codec
