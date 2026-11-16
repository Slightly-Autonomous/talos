// Rule dry-run over history (ADR 0012). Replays the journal with the same
// reducer and matcher the engine uses, so a preview can never disagree with
// what the engine would have done. Nothing is dispatched.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/domain/property.h"
#include "core/journal/journal.h"
#include "core/rules/rules.h"

namespace talos {

struct PreviewMatch {
  std::uint64_t journal_seq = 0;
  Timestamp received_at;
  EventId event_id;
  std::string event_type;
  DeviceId device;
};

struct PreviewResult {
  std::vector<PreviewMatch> matches;
  std::uint32_t evaluated_observations = 0;
};

// Evaluates `rule` against observations received in [from, to], using the arm
// mode in force at each observation and engine semantics (only observations
// that became current state can trigger).
PreviewResult preview_rule(const Rule& rule, const Property& property,
                           const std::vector<JournalEntry>& journal, Timestamp from, Timestamp to,
                           const std::function<std::uint16_t(Timestamp)>& local_minute_of_day);

}  // namespace talos
