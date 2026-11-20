#include "core/ingest/ingestor.h"

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
  return true;
}

}  // namespace

IngestResult Ingestor::ingest(const ObservationEnvelope& env, Timestamp received_at) {
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

  const auto check = tracker_.check(env.source.device, env.position);
  switch (check.verdict) {
    case SourceTracker::Verdict::kDuplicate:
      return {IngestStatus::kDuplicate, std::nullopt, {}};
    case SourceTracker::Verdict::kTooOld:
      return {IngestStatus::kTooOld, std::nullopt, {}};
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
    }
  }
}

}  // namespace talos
