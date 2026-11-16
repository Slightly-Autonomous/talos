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
};

class Ingestor {
 public:
  Ingestor(const Property& property, IJournal& journal) : property_(property), journal_(journal) {}

  IngestResult ingest(const ObservationEnvelope& env, Timestamp received_at);

  // Rebuilds dedup state from an existing journal (hub restart).
  void restore();

 private:
  const Property& property_;
  IJournal& journal_;
  SourceTracker tracker_;
};

}  // namespace talos
