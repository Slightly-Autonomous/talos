#include "core/state/state.h"

namespace talos {

namespace {

struct Reducer {
  PropertyState& s;
  const JournalEntry& entry;

  void operator()(const ObservationRecorded& rec) const {
    const auto& env = rec.envelope;
    auto it = s.capabilities.find(env.source);
    if (it != s.capabilities.end() && env.position < it->second.position) {
      return;  // older than what we hold from this source; history only
    }
    s.capabilities[env.source] =
        CapabilityState{env.payload,     env.event_type,    env.event_id,     env.position,
                        env.observed_at, entry.received_at, entry.journal_seq};
  }

  void operator()(const CommandTransition& t) const { s.commands.apply(t); }
  void operator()(const ArmModeChanged& m) const { s.arm_mode = m.mode; }
  void operator()(const ConfigRevisionApplied& c) const { s.config_revision = c.revision; }
};

}  // namespace

PropertyState reduce(PropertyState state, const JournalEntry& entry) {
  if (entry.journal_seq <= state.through_seq) return state;  // already applied
  std::visit(Reducer{state, entry}, entry.event);
  state.through_seq = entry.journal_seq;
  return state;
}

PropertyState replay(PropertyState snapshot, const IJournal& journal) {
  for (const auto& e : read_all(journal, snapshot.through_seq)) {
    snapshot = reduce(std::move(snapshot), e);
  }
  return snapshot;
}

PropertyState state_at(const IJournal& journal, Timestamp t) {
  PropertyState s;
  for (const auto& e : read_all(journal)) {
    if (e.received_at > t) break;
    s = reduce(std::move(s), e);
  }
  return s;
}

Freshness freshness(const PropertyState& state, const CapabilityRef& ref, Timestamp now,
                    Millis max_age) {
  auto it = state.capabilities.find(ref);
  if (it == state.capabilities.end()) return Freshness::kUnknown;
  // received_at ahead of now means the hub clock stepped back (ADR 0008 keeps
  // received_at non-decreasing). The real age is unknown, so fail closed.
  if (it->second.received_at > now) return Freshness::kStale;
  return (now - it->second.received_at) > max_age ? Freshness::kStale : Freshness::kFresh;
}

}  // namespace talos
