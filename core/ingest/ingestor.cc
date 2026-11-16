#include "core/ingest/ingestor.h"

namespace talos {

IngestResult Ingestor::ingest(const ObservationEnvelope& env, Timestamp received_at) {
  if (Status s = validate_envelope(env); !s) {
    return {IngestStatus::kRejected, std::nullopt, s.error()};
  }
  // Ownership boundary: evidence must come from a device of this property.
  if (env.property != property_.id) {
    return {IngestStatus::kRejected, std::nullopt,
            Error{"cross_property_observation", "envelope for property " + env.property.str()}};
  }
  if (!property_.find_device(env.source.device)) {
    return {IngestStatus::kRejected, std::nullopt,
            Error{"unknown_device", "device " + env.source.device.str()}};
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
