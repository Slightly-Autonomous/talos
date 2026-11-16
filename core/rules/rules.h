// Minimal rule engine v0 (ADR 0007): event-type/zone triggers, arming-mode and
// time-window conditions, typed actions -> Intents.
//
// Matching is a pure function. Intent keys are deterministic
// (rule id, revision, causation event, action index) and become the command
// idempotency key, so re-evaluating the same event can never produce a second
// physical effect.
//
// Time-zone and DST math stays outside core: callers pass the property-local
// minute of day.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/domain/property.h"
#include "core/events/observation.h"
#include "core/execution/command.h"
#include "core/time/time.h"

namespace talos {

enum class ArmMode : std::uint8_t { kDisarmed, kArmedHome, kArmedAway };

// Local minutes of day, [start, end). Wraps past midnight when start > end.
struct TimeWindow {
  std::uint16_t start_minute = 0;
  std::uint16_t end_minute = 0;
  bool contains(std::uint16_t minute) const;
};

struct RuleTrigger {
  std::string event_type;
  std::optional<ZoneId> zone;  // empty = any zone
};

struct RuleConditions {
  std::vector<ArmMode> armed_modes;  // empty = any mode
  std::optional<TimeWindow> schedule;
};

struct RuleAction {
  std::string operation;
  CapabilityRef target;
  CommandArgs args;
  Millis ttl{5'000};
  VerificationMode verification = VerificationMode::kObservedState;
  Millis verification_window{5'000};
  std::optional<ExpectedObservation> expect;
};

struct Rule {
  RuleId id;
  std::uint32_t revision = 1;
  PropertyId property;
  PrincipalId principal;  // authority the rule acts under (a kRule principal)
  bool enabled = true;
  RuleTrigger trigger;
  RuleConditions conditions;
  std::vector<RuleAction> actions;
};

struct RuleContext {
  ArmMode mode = ArmMode::kDisarmed;
  std::uint16_t local_minute_of_day = 0;
};

struct Intent {
  std::string key;  // deterministic; used as idempotency key
  RuleId rule;
  std::uint32_t rule_revision = 0;
  PrincipalId principal;
  PropertyId property;
  EventId causation;
  RuleAction action;
};

std::vector<Intent> evaluate_rules(const std::vector<Rule>& rules, const Property& property,
                                   const ObservationEnvelope& observation,
                                   const RuleContext& context);

// Turns an intent into a command request created at `now`.
Command to_command(const Intent& intent, Timestamp now);

}  // namespace talos
