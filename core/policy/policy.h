// Authorization policy for command requests (spec §23, §45).
//
// Deny by default. This is the place where cross-property ownership is
// enforced for commands: a principal can act only on properties it holds a
// grant for, and only up to the grant's risk ceiling.
#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "core/domain/ids.h"
#include "core/domain/property.h"
#include "core/execution/command.h"
#include "core/time/time.h"

namespace talos {

enum class PrincipalKind { kHuman, kAgent, kRule, kSystem };

struct Grant {
  PropertyId property;
  RiskClass max_risk = RiskClass::kInformational;
};

struct Principal {
  PrincipalId id;
  PrincipalKind kind = PrincipalKind::kHuman;
  std::vector<Grant> grants;
};

struct PolicyContext {
  Timestamp now;
  // Whether the target's current state is fresh. Unknown counts as not fresh.
  std::optional<bool> target_state_fresh;
};

// Risk class is derived from the operation, never taken from the caller.
// Unknown operations are treated as security-critical (fail closed).
RiskClass risk_class_for(std::string_view operation);

// Highest risk a non-human principal may ever request, regardless of grants.
inline constexpr RiskClass kMaxNonHumanRisk = RiskClass::kDisruptiveOutput;

Authorization evaluate(const Principal& principal, const Command& command,
                       const PolicyContext& context);

// Raising protection (disarmed < armed_home < armed_away) needs a grant of at
// least benign_output. Lowering it (disarming, or away -> home, which turns
// away-only rules off) is security-critical: human principals only, with a
// security_critical grant.
Authorization authorize_arm_change(const Principal& principal, const PropertyId& property,
                                   ArmMode current, ArmMode target);

// Changing layout or rules can silence alarms: security-critical, humans only.
Authorization authorize_config_change(const Principal& principal, const PropertyId& property);

}  // namespace talos
