#include "core/ingest/ingestor.h"

#include <string>

namespace talos {

namespace {

// Payload kind each known capability reports. Capabilities not listed here
// are not checked (adapters may add new ones), but a known capability can
// never carry another capability's payload (a PIR cannot report lock state).
bool payload_fits(const CapabilityId& capability, const ObservationPayload& p) {
  const std::string& c = capability.str();
  if (c == "motion.detect") return std::holds_alternative<MotionState>(p);
  if (c == "contact.open_state") return std::holds_alternative<ContactState>(p);
  if (c == "impact.detect") return std::holds_alternative<ImpactDetected>(p);
  if (c == "environment.temperature_humidity")
    return std::holds_alternative<TemperatureHumidity>(p);
  if (c == "access.lock") return std::holds_alternative<LockState>(p);
  if (c == "audio.speak") return std::holds_alternative<AudioPlayback>(p);
  if (c == "camera.detection") return std::holds_alternative<ObjectDetected>(p);
  if (c == "device.health") return std::holds_alternative<Heartbeat>(p);
  if (c == "tamper.enclosure") return std::holds_alternative<TamperState>(p);
  return true;
}

// Rules trigger on event_type while the UI renders the payload, so the two
// must agree: each payload kind has a fixed set of event types, and a value
// specific type ("lock.unlocked") must match the value.
bool event_type_fits(const std::string& t, const ObservationPayload& p) {
  if (const auto* m = std::get_if<MotionState>(&p)) {
    return t == (m->detected ? "motion.detected" : "motion.cleared");
  }
  if (const auto* c = std::get_if<ContactState>(&p)) {
    return t == "contact.state" || t == (c->open ? "contact.opened" : "contact.closed");
  }
  if (const auto* l = std::get_if<LockState>(&p)) {
    return t == "lock.state" || t == (l->locked ? "lock.locked" : "lock.unlocked");
  }
  if (const auto* a = std::get_if<AudioPlayback>(&p)) {
    return t == "audio.playback" ||
           t == (a->playing ? "audio.playback_started" : "audio.playback_stopped");
  }
  if (std::holds_alternative<ImpactDetected>(p)) return t == "impact.detected";
  if (std::holds_alternative<TemperatureHumidity>(p)) return t == "environment.reading";
  if (std::holds_alternative<ObjectDetected>(p)) return t == "camera.object_detected";
  if (std::holds_alternative<Heartbeat>(p)) return t == "device.heartbeat";
  if (const auto* x = std::get_if<TamperState>(&p)) {
    return t == "tamper.state" || t == (x->open ? "tamper.opened" : "tamper.closed");
  }
  return false;
}

}  // namespace

IngestResult Ingestor::ingest(const ObservationEnvelope& env, Timestamp received_at,
                              IngestOptions options) {
  if (Status s = validate_envelope(env); !s) {
    return {IngestStatus::kRejected, std::nullopt, s.error()};
  }
  // Ownership boundary: evidence must come from a device of this property.
  if (env.property != property_.id) {
    return {IngestStatus::kRejected, std::nullopt,
            Error{"cross_property_observation", "envelope for property " + env.property.str()}};
  }
  const Device* device = property_.find_device(env.source.device);
  if (!device) {
    return {IngestStatus::kRejected, std::nullopt,
            Error{"unknown_device", "device " + env.source.device.str()}};
  }
  // An event id names its source ("<device>:..."): rule command keys and the
  // maintenance re-evaluation are keyed on it, so a device that could use
  // another's ids could pre-claim them and make that device's alarm dedupe
  // to a command it already spent.
  {
    const std::string& dev = env.source.device.str();
    const std::string& id = env.event_id.str();
    if (id.size() <= dev.size() + 1 || id.compare(0, dev.size(), dev) != 0 ||
        id[dev.size()] != ':') {
      return {IngestStatus::kRejected, std::nullopt,
              Error{"event_id_not_owned_by_source", "event_id must start with \"" + dev + ":\""}};
    }
  }
  if (!device->declares(env.source.capability)) {
    return {IngestStatus::kRejected, std::nullopt,
            Error{"undeclared_capability",
                  env.source.device.str() + " does not declare " + env.source.capability.str()}};
  }
  if (!payload_fits(env.source.capability, env.payload)) {
    return {IngestStatus::kRejected, std::nullopt,
            Error{"payload_does_not_match_capability",
                  "payload kind does not fit " + env.source.capability.str()}};
  }

  if (!event_type_fits(env.event_type, env.payload)) {
    return {IngestStatus::kRejected, std::nullopt,
            Error{"event_type_does_not_match_payload",
                  "event_type " + env.event_type + " does not fit the payload"}};
  }

  const auto check = tracker_.check(env.source.device, env.position, options.epoch_ceiling);
  switch (check.verdict) {
    case SourceTracker::Verdict::kDuplicate:
      return {IngestStatus::kDuplicate, std::nullopt, {}};
    case SourceTracker::Verdict::kTooOld:
      return {IngestStatus::kTooOld, std::nullopt, {}};
    case SourceTracker::Verdict::kOutOfRange:
      return {
          IngestStatus::kRejected, std::nullopt,
          Error{"source_position_out_of_range",
                "boot epoch " + std::to_string(env.position.boot_epoch) + " / sequence " +
                    std::to_string(env.position.sequence) +
                    " jumps too far past what the hub has seen from " + env.source.device.str()}};
    case SourceTracker::Verdict::kNew:
      break;
  }

  auto appended = journal_.append(received_at, ObservationRecorded{env, check.order});
  if (!appended) return {IngestStatus::kRejected, std::nullopt, appended.error()};
  tracker_.record(env.source.device, env.position);
  return {IngestStatus::kAccepted, std::move(appended).value(), {}};
}

void Ingestor::restore() {
  for (const auto& entry : read_all(journal_)) {
    if (const auto* obs = std::get_if<ObservationRecorded>(&entry.event)) {
      tracker_.record(obs->envelope.source.device, obs->envelope.position);
    } else if (const auto* reset = std::get_if<SourceReset>(&entry.event)) {
      tracker_.reset(reset->device);
    }
  }
}

}  // namespace talos
