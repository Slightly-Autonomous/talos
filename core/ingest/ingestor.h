// Ingestion boundary for observations: validate, check ownership, dedupe,
// classify source order, append to the journal.
#pragma once

#include <optional>

#include "core/base/result.h"
#include "core/domain/property.h"
#include "core/events/observation.h"
#include "core/ingest/source_tracker.h"
#include "core/journal/journal.h"

namespace talos {

enum class IngestStatus : std::uint8_t {
  kAccepted,   // journaled
  kDuplicate,  // already journaled; no effect
  kTooOld,     // outside the dedup window; refused to avoid duplicate effects
  kRejected,   // invalid or not owned by this property
};

struct IngestResult {
  IngestStatus status = IngestStatus::kRejected;
  std::optional<JournalEntry> entry;  // when accepted
  std::optional<Error> error;         // when rejected
  // Accepted, but the capability already held a newer source position, so
  // the observation is history only: it neither verifies commands nor
  // triggers rules (set by HubEngine).
  bool superseded = false;
  // The position held when `superseded` (for health: a device whose real
  // reports keep landing behind its record needs an owner reset).
  std::optional<SourcePosition> held_position = std::nullopt;
  // Rule actions whose command id was held by another principal and that
  // were recorded under a fallback id instead (set by HubEngine).
  std::uint32_t rule_conflicts = 0;
};

struct IngestOptions {
  // Highest boot epoch accepted regardless of the jump bound; see
  // SourceTracker. 0 means no ceiling (counter epochs only).
  std::uint32_t epoch_ceiling = 0;
};

class Ingestor {
 public:
  Ingestor(const Property& property, IJournal& journal) : property_(property), journal_(journal) {}

  IngestResult ingest(const ObservationEnvelope& env, Timestamp received_at,
                      IngestOptions options = {});

  // Rebuilds dedup state from an existing journal (hub restart).
  void restore();
  // Whether the position is unseen and in range (read-only; a replayed
  // position is not new).
  bool is_new(const DeviceId& device, SourcePosition pos) const {
    return tracker_.check(device, pos).verdict == SourceTracker::Verdict::kNew;
  }
  // Forgets the device's source positions (after a journaled SourceReset).
  void reset(const DeviceId& device) { tracker_.reset(device); }

 private:
  const Property& property_;
  IJournal& journal_;
  SourceTracker tracker_;
};

}  // namespace talos
