#include "core/engine/preview.h"

#include "core/state/state.h"

namespace talos {

PreviewResult preview_rule(const Rule& rule, const Property& property,
                           const std::vector<JournalEntry>& journal, Timestamp from, Timestamp to,
                           const std::function<std::uint16_t(Timestamp)>& local_minute_of_day) {
  PreviewResult out;
  PropertyState s;
  Rule candidate = rule;
  candidate.enabled = true;  // previewing a disabled draft is the common case
  for (const auto& entry : journal) {
    s = reduce(std::move(s), entry);
    const auto* obs = std::get_if<ObservationRecorded>(&entry.event);
    if (!obs || entry.received_at < from || entry.received_at > to) continue;
    ++out.evaluated_observations;
    auto it = s.capabilities.find(obs->envelope.source);
    if (it == s.capabilities.end() || it->second.journal_seq != entry.journal_seq) continue;
    const RuleContext ctx{s.arm_mode, local_minute_of_day ? local_minute_of_day(entry.received_at)
                                                          : std::uint16_t{0}};
    if (!evaluate_rules({candidate}, property, obs->envelope, ctx).empty()) {
      out.matches.push_back({entry.journal_seq, entry.received_at, obs->envelope.event_id,
                             obs->envelope.event_type, obs->envelope.source.device});
    }
  }
  return out;
}

}  // namespace talos
