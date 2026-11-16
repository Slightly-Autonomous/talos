// Property state projection: S(t) = Reduce(S0, entries in journal_seq order).
//
// The reducer is pure and deterministic. It consumes journal entries strictly
// in journal_seq order and ignores entries at or below `through_seq`, so
// re-applying an entry is a no-op.
//
// Observed capability state keeps the latest value by SOURCE position for that
// capability: a reordered (late) observation is journaled and visible in
// history, but it does not overwrite a newer value from the same device.
//
// Inferred state (occupancy, movement) arrives with the twin in Phase 2.
#pragma once

#include <cstdint>
#include <map>
#include <optional>

#include "core/domain/capability.h"
#include "core/events/observation.h"
#include "core/execution/ledger.h"
#include "core/journal/journal.h"
#include "core/time/time.h"

namespace talos {

struct CapabilityState {
  ObservationPayload value;
  std::string event_type;
  EventId last_event;
  SourcePosition position;
  std::optional<Timestamp> observed_at;
  Timestamp received_at;
  std::uint64_t journal_seq = 0;
  bool operator==(const CapabilityState&) const = default;
};

// Doubles as a snapshot: `through_seq` is the last journal entry it covers.
struct PropertyState {
  std::uint64_t through_seq = 0;
  std::map<CapabilityRef, CapabilityState> capabilities;
  CommandLedger commands;
  ArmMode arm_mode = ArmMode::kDisarmed;
  std::uint32_t config_revision = 0;  // 0: none journaled yet
  bool operator==(const PropertyState&) const = default;
};

PropertyState reduce(PropertyState state, const JournalEntry& entry);

// Applies every journal entry after state.through_seq.
PropertyState replay(PropertyState snapshot, const IJournal& journal);

// State as of hub time t: the journal prefix with received_at <= t.
// (received_at is non-decreasing in journal order, so this is a prefix.)
PropertyState state_at(const IJournal& journal, Timestamp t);

enum class Freshness : std::uint8_t {
  kUnknown,  // never reported: not "clear", not "healthy"
  kFresh,
  kStale,  // last report older than max_age
};

Freshness freshness(const PropertyState& state, const CapabilityRef& ref, Timestamp now,
                    Millis max_age);

}  // namespace talos
