#include "core/policy/policy.h"

#include <string>
#include <utility>

#include "core/execution/operations.h"

namespace talos {

RiskClass risk_class_for(std::string_view op) {
  // The operation catalog (core/execution/operations.h) is the single source.
  const OperationSpec* spec = find_operation(op);
  return spec ? spec->risk : RiskClass::kSecurityCritical;
}

Authorization evaluate(const Principal& principal, const Command& command,
                       const PolicyContext& ctx) {
  auto deny = [](std::string reason) { return Authorization{false, std::move(reason)}; };

  if (principal.id.empty() || principal.id != command.principal) {
    return deny("principal_mismatch");
  }
  if (ctx.now >= command.expires_at) return deny("expired");

  const Grant* grant = nullptr;
  for (const auto& g : principal.grants) {
    if (g.property == command.property) grant = &g;
  }
  if (!grant) return deny("no_grant_for_property");

  const RiskClass risk = risk_class_for(command.operation);
  if (risk > grant->max_risk) return deny("risk_exceeds_grant");
  if (principal.kind != PrincipalKind::kHuman && risk > kMaxNonHumanRisk) {
    return deny("requires_human_authorization");
  }
  if (risk == RiskClass::kSecurityCritical && ctx.target_state_fresh != true) {
    return deny("target_state_not_fresh");
  }
  return {true, "granted"};
}

Authorization authorize_arm_change(const Principal& principal, const PropertyId& property,
                                   ArmMode current, ArmMode target) {
  const Grant* grant = nullptr;
  for (const auto& g : principal.grants) {
    if (g.property == property) grant = &g;
  }
  if (principal.id.empty() || !grant) return {false, "no_grant_for_property"};
  // ArmMode is ordered by protection level.
  if (static_cast<int>(target) < static_cast<int>(current)) {
    if (principal.kind != PrincipalKind::kHuman) return {false, "requires_human_authorization"};
    if (grant->max_risk < RiskClass::kSecurityCritical) return {false, "risk_exceeds_grant"};
    return {true, "granted"};
  }
  if (grant->max_risk < RiskClass::kBenignOutput) return {false, "risk_exceeds_grant"};
  return {true, "granted"};
}

Authorization authorize_config_change(const Principal& principal, const PropertyId& property) {
  const Grant* grant = nullptr;
  for (const auto& g : principal.grants) {
    if (g.property == property) grant = &g;
  }
  if (principal.id.empty() || !grant) return {false, "no_grant_for_property"};
  if (principal.kind != PrincipalKind::kHuman) return {false, "requires_human_authorization"};
  if (grant->max_risk < RiskClass::kSecurityCritical) return {false, "risk_exceeds_grant"};
  return {true, "granted"};
}

}  // namespace talos
