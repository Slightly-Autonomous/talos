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

TEST(Policy, ArmingAndDisarming) {
  const PropertyId house("house-01");
  Principal agent{
      PrincipalId("agent-1"), PrincipalKind::kAgent, {{house, RiskClass::kSecurityCritical}}};
  EXPECT_TRUE(authorize_arm_change(agent, house, ArmMode::kDisarmed, ArmMode::kArmedAway).allowed);
  EXPECT_EQ(authorize_arm_change(agent, house, ArmMode::kArmedAway, ArmMode::kDisarmed).reason,
            "requires_human_authorization");
  EXPECT_TRUE(
      authorize_arm_change(owner(), house, ArmMode::kArmedAway, ArmMode::kDisarmed).allowed);
  EXPECT_EQ(
      authorize_arm_change(owner(), PropertyId("house-02"), ArmMode::kDisarmed, ArmMode::kArmedAway)
          .reason,
      "no_grant_for_property");
}

// Red team #7: away -> home turns away-only rules off, so it is a disarm for
// policy purposes. Non-humans may only raise protection.
TEST(Policy, AgentCannotLowerArmMode) {
  const PropertyId house("house-01");
  Principal agent{
      PrincipalId("agent-1"), PrincipalKind::kAgent, {{house, RiskClass::kSecurityCritical}}};
  EXPECT_EQ(authorize_arm_change(agent, house, ArmMode::kArmedAway, ArmMode::kArmedHome).reason,
            "requires_human_authorization");
  EXPECT_TRUE(authorize_arm_change(agent, house, ArmMode::kArmedHome, ArmMode::kArmedAway).allowed);
  EXPECT_TRUE(authorize_arm_change(agent, house, ArmMode::kDisarmed, ArmMode::kArmedHome).allowed);
  EXPECT_TRUE(authorize_arm_change(agent, house, ArmMode::kArmedHome, ArmMode::kArmedHome).allowed);
  // A human needs a security-critical grant to lower it.
  Principal helper = owner();
  helper.grants[0].max_risk = RiskClass::kDisruptiveOutput;
  EXPECT_EQ(authorize_arm_change(helper, house, ArmMode::kArmedAway, ArmMode::kArmedHome).reason,
            "risk_exceeds_grant");
  EXPECT_TRUE(
      authorize_arm_change(owner(), house, ArmMode::kArmedAway, ArmMode::kArmedHome).allowed);
}

TEST(Policy, ConfigChangesAreHumanSecurityCritical) {
  EXPECT_TRUE(authorize_config_change(owner(), PropertyId("house-01")).allowed);
  Principal agent{PrincipalId("agent-1"),
                  PrincipalKind::kAgent,
                  {{PropertyId("house-01"), RiskClass::kSecurityCritical}}};
  EXPECT_EQ(authorize_config_change(agent, PropertyId("house-01")).reason,
            "requires_human_authorization");
  Principal viewer = owner();
  viewer.grants[0].max_risk = RiskClass::kDisruptiveOutput;
  EXPECT_EQ(authorize_config_change(viewer, PropertyId("house-01")).reason, "risk_exceeds_grant");
  EXPECT_EQ(authorize_config_change(owner(), PropertyId("house-02")).reason,
            "no_grant_for_property");
}

TEST(Policy, ExpiredRequestDenied) {
  EXPECT_EQ(evaluate(owner(), cmd("notify"), {kNow + Millis{5'000}, true}).reason, "expired");
}

// ADR 0020: camera images are for people with at least a sensitive_read grant.
TEST(Policy, MediaIsForHumansWithASensitiveReadGrant) {
  EXPECT_TRUE(authorize_media_view(owner(), PropertyId("house-01")).allowed);
  Principal viewer = owner();
  viewer.grants[0].max_risk = RiskClass::kSensitiveRead;
  EXPECT_TRUE(authorize_media_view(viewer, PropertyId("house-01")).allowed);
  viewer.grants[0].max_risk = RiskClass::kInformational;
  EXPECT_EQ(authorize_media_view(viewer, PropertyId("house-01")).reason, "risk_exceeds_grant");
  Principal agent{PrincipalId("agent-1"),
                  PrincipalKind::kAgent,
                  {{PropertyId("house-01"), RiskClass::kDisruptiveOutput}}};
  EXPECT_EQ(authorize_media_view(agent, PropertyId("house-01")).reason,
            "requires_human_authorization");
  EXPECT_EQ(authorize_media_view(owner(), PropertyId("house-02")).reason, "no_grant_for_property");
}

}  // namespace
}  // namespace talos
