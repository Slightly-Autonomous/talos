#include "core/policy/policy.h"

#include <string>
#include <utility>

namespace talos {

RiskClass risk_class_for(std::string_view op) {
  struct Entry {
    std::string_view operation;
    RiskClass risk;
  };
  // Phase 1 catalog. Grows with capabilities; anything absent fails closed.
  static constexpr Entry kCatalog[] = {
      {"state.read", RiskClass::kInformational},
      {"camera.snapshot", RiskClass::kSensitiveRead},
      {"notify", RiskClass::kBenignOutput},
      {"light.set_on", RiskClass::kBenignOutput},
      {"audio.speak", RiskClass::kDisruptiveOutput},
      {"alarm.activate", RiskClass::kDisruptiveOutput},
      {"access.set_locked", RiskClass::kSecurityCritical},
      {"alarm.disarm", RiskClass::kSecurityCritical},
  };
  for (const auto& e : kCatalog) {
    if (e.operation == op) return e.risk;
  }
  return RiskClass::kSecurityCritical;
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
                                   ArmMode target) {
  const Grant* grant = nullptr;
  for (const auto& g : principal.grants) {
    if (g.property == property) grant = &g;
  }
  if (principal.id.empty() || !grant) return {false, "no_grant_for_property"};
  if (target == ArmMode::kDisarmed) {
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
