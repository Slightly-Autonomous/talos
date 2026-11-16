// The hub's append-only journal (ADR 0008).
//
// Ordering authority: every accepted entry gets a strictly increasing
// `journal_seq`. Replay and projection iterate in journal_seq order only.
// Device clocks (observed_at) and per-device source positions never order
// entries across devices.
//
// `received_at` is the hub's receipt time, kept non-decreasing in journal
// order. If the hub clock steps backwards, the entry keeps the previous
// received_at and is flagged `clock_regressed`, so "state at time t" is always
// a clean journal prefix.
#pragma once

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "core/base/result.h"
#include "core/domain/property.h"
#include "core/events/observation.h"
#include "core/execution/command.h"
#include "core/time/time.h"

namespace talos {

// How an observation's source position related to what the hub had already
// seen from that device when it was ingested.
enum class SourceOrder : std::uint8_t {
  kInOrder,  // next expected sequence
  kGap,      // sequence jumped ahead; earlier events missing (so far)
  kLate,     // older than the highest seen; filled a gap or arrived reordered
  kNewBoot,  // first event of a newer boot epoch
};

struct ObservationRecorded {
  ObservationEnvelope envelope;
  SourceOrder order = SourceOrder::kInOrder;
  bool operator==(const ObservationRecorded&) const = default;
};

// Arming changes are journaled so replay reproduces rule behavior.
struct ArmModeChanged {
  ArmMode mode = ArmMode::kDisarmed;
  PrincipalId principal;
  bool operator==(const ArmModeChanged&) const = default;
};

using DomainEvent = std::variant<ObservationRecorded, CommandTransition, ArmModeChanged>;

struct JournalEntry {
  std::uint64_t journal_seq = 0;  // 1-based, strictly increasing
  Timestamp received_at;
  bool clock_regressed = false;
  DomainEvent event;
  bool operator==(const JournalEntry&) const = default;
};

class IJournal {
 public:
  virtual ~IJournal() = default;
  // Assigns journal_seq and normalizes received_at; returns the stored entry.
  virtual Result<JournalEntry> append(Timestamp received_at, DomainEvent event) = 0;
  // Entries with journal_seq > after_seq, in order, at most `limit`.
  virtual std::vector<JournalEntry> read(std::uint64_t after_seq, std::size_t limit) const = 0;
  virtual std::uint64_t last_seq() const = 0;
};

// Reference implementation for tests and the twin. SQLite comes in Phase 3
// behind the same interface and must pass the same tests.
class InMemoryJournal final : public IJournal {
 public:
  Result<JournalEntry> append(Timestamp received_at, DomainEvent event) override;
  std::vector<JournalEntry> read(std::uint64_t after_seq, std::size_t limit) const override;
  std::uint64_t last_seq() const override { return entries_.size(); }

 private:
  std::vector<JournalEntry> entries_;
};

// Reads the whole journal after `after_seq` (convenience for replay).
std::vector<JournalEntry> read_all(const IJournal& journal, std::uint64_t after_seq = 0);

}  // namespace talos
