#include "core/policy/policy.h"

#include <gtest/gtest.h>

namespace talos {
namespace {

constexpr Timestamp kNow{10'000};

Command cmd(std::string op, std::string principal = "user-33") {
  Command c;
  c.id = CommandId("c1");
  c.principal = PrincipalId(std::move(principal));
  c.property = PropertyId("house-01");
  c.operation = std::move(op);
  c.expires_at = kNow + Millis{5'000};
  return c;
}

Principal owner() {
  return {PrincipalId("user-33"),
          PrincipalKind::kHuman,
          {{PropertyId("house-01"), RiskClass::kSecurityCritical}}};
}

TEST(Policy, DenyByDefaultWithoutGrant) {
  Principal p{PrincipalId("user-33"), PrincipalKind::kHuman, {}};
  EXPECT_EQ(evaluate(p, cmd("notify"), {kNow, true}).reason, "no_grant_for_property");
}

TEST(Policy, GrantForOtherPropertyDoesNotApply) {
  Principal p = owner();
  p.grants = {{PropertyId("house-02"), RiskClass::kSecurityCritical}};
  auto a = evaluate(p, cmd("audio.speak"), {kNow, true});
  EXPECT_FALSE(a.allowed);
  EXPECT_EQ(a.reason, "no_grant_for_property");
}

TEST(Policy, PrincipalMustMatchCommand) {
  EXPECT_EQ(evaluate(owner(), cmd("notify", "someone-else"), {kNow, true}).reason,
            "principal_mismatch");
}

TEST(Policy, RiskCeilingEnforced) {
  Principal p = owner();
  p.grants[0].max_risk = RiskClass::kBenignOutput;
  EXPECT_TRUE(evaluate(p, cmd("notify"), {kNow, true}).allowed);
  EXPECT_EQ(evaluate(p, cmd("audio.speak"), {kNow, true}).reason, "risk_exceeds_grant");
}

TEST(Policy, AgentsAndRulesCannotDoSecurityCriticalEvenWithGrant) {
  Principal agent{PrincipalId("agent-1"),
                  PrincipalKind::kAgent,
                  {{PropertyId("house-01"), RiskClass::kSecurityCritical}}};
  EXPECT_EQ(evaluate(agent, cmd("access.set_locked", "agent-1"), {kNow, true}).reason,
            "requires_human_authorization");
  EXPECT_TRUE(evaluate(agent, cmd("audio.speak", "agent-1"), {kNow, true}).allowed);
}

TEST(Policy, SecurityCriticalRequiresFreshTargetState) {
  EXPECT_EQ(evaluate(owner(), cmd("access.set_locked"), {kNow, std::nullopt}).reason,
            "target_state_not_fresh");
  EXPECT_EQ(evaluate(owner(), cmd("access.set_locked"), {kNow, false}).reason,
            "target_state_not_fresh");
  EXPECT_TRUE(evaluate(owner(), cmd("access.set_locked"), {kNow, true}).allowed);
}

TEST(Policy, UnknownOperationFailsClosed) {
  EXPECT_EQ(risk_class_for("robot.do_anything"), RiskClass::kSecurityCritical);
  Principal p = owner();
  p.grants[0].max_risk = RiskClass::kDisruptiveOutput;
  EXPECT_FALSE(evaluate(p, cmd("robot.do_anything"), {kNow, true}).allowed);
}

TEST(Policy, ExpiredRequestDenied) {
  EXPECT_EQ(evaluate(owner(), cmd("notify"), {kNow + Millis{5'000}, true}).reason, "expired");
}

}  // namespace
}  // namespace talos
