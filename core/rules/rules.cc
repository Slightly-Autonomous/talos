#include "core/rules/rules.h"

#include <algorithm>

namespace talos {

bool TimeWindow::contains(std::uint16_t minute) const {
  if (start_minute == end_minute) return true;  // whole day
  if (start_minute < end_minute) return minute >= start_minute && minute < end_minute;
  return minute >= start_minute || minute < end_minute;  // wraps midnight
}

namespace {

bool matches(const Rule& rule, const Property& property, const ObservationEnvelope& obs,
             const RuleContext& ctx) {
  if (!rule.enabled || rule.property != obs.property) return false;
  if (rule.trigger.event_type != obs.event_type) return false;
  if (rule.trigger.zone && property.zone_of(obs.source) != rule.trigger.zone) return false;
  const auto& modes = rule.conditions.armed_modes;
  if (!modes.empty() && std::find(modes.begin(), modes.end(), ctx.mode) == modes.end()) {
    return false;
  }
  if (rule.conditions.schedule && !rule.conditions.schedule->contains(ctx.local_minute_of_day)) {
    return false;
  }
  return true;
}

}  // namespace

std::vector<Intent> evaluate_rules(const std::vector<Rule>& rules, const Property& property,
                                   const ObservationEnvelope& obs, const RuleContext& ctx) {
  std::vector<Intent> out;
  for (const auto& rule : rules) {
    if (!matches(rule, property, obs, ctx)) continue;
    for (std::size_t i = 0; i < rule.actions.size(); ++i) {
      Intent intent;
      intent.key = rule.id.str() + "@" + std::to_string(rule.revision) + ":" + obs.event_id.str() +
                   ":" + std::to_string(i);
      intent.rule = rule.id;
      intent.rule_revision = rule.revision;
      intent.principal = rule.principal;
      intent.property = rule.property;
      intent.causation = obs.event_id;
      intent.action = rule.actions[i];
      out.push_back(std::move(intent));
    }
  }
  return out;
}

Command to_command(const Intent& intent, Timestamp now) {
  Command c;
  c.id = CommandId("cmd:" + intent.key);
  c.idempotency_key = intent.key;
  c.principal = intent.principal;
  c.property = intent.property;
  c.target = intent.action.target;
  c.operation = intent.action.operation;
  c.args = intent.action.args;
  c.created_at = now;
  c.expires_at = now + intent.action.ttl;
  c.verification = intent.action.verification;
  c.verification_window = intent.action.verification_window;
  c.expect = intent.action.expect;
  c.causation = intent.causation;
  return c;
}

}  // namespace talos
