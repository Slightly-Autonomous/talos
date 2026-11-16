#include "core/rules/rules.h"

#include <gtest/gtest.h>

#include "core/testing/fixtures.h"

namespace talos {
namespace {

const CapabilityRef kSpeaker{DeviceId("patio_speaker"), std::nullopt, CapabilityId("audio.speak")};

Rule backyard_rule() {
  Rule r;
  r.id = RuleId("backyard_motion_warning");
  r.revision = 3;
  r.property = PropertyId("house-01");
  r.principal = PrincipalId("rule:backyard_motion_warning");
  r.trigger = {"motion.detected", ZoneId("backyard")};
  r.conditions.armed_modes = {ArmMode::kArmedAway};
  r.conditions.schedule = TimeWindow{18 * 60, 6 * 60};  // sunset-ish to sunrise-ish
  RuleAction speak;
  speak.operation = "audio.speak";
  speak.target = kSpeaker;
  speak.args = AudioSpeak{"This property is monitored.", 65};
  speak.expect = ExpectedObservation{kSpeaker, AudioPlayback{true}};
  r.actions.push_back(speak);
  return r;
}

const Property kProperty = testing::sample_property();
constexpr std::uint16_t k2am = 2 * 60;

TEST(Rules, FiresOnBackyardMotionWhenArmedAwayAt2am) {
  auto intents = evaluate_rules({backyard_rule()}, kProperty, testing::motion("node-07", 1, 1),
                                {ArmMode::kArmedAway, k2am});
  ASSERT_EQ(intents.size(), 1u);
  EXPECT_EQ(intents[0].action.operation, "audio.speak");
  EXPECT_EQ(intents[0].causation, EventId("node-07:1:1"));
  EXPECT_EQ(intents[0].key, "backyard_motion_warning@3:node-07:1:1:0");
}

TEST(Rules, DoesNotFireWhenDisarmedOrOutsideWindow) {
  auto motion = testing::motion("node-07", 1, 1);
  EXPECT_TRUE(
      evaluate_rules({backyard_rule()}, kProperty, motion, {ArmMode::kDisarmed, k2am}).empty());
  EXPECT_TRUE(
      evaluate_rules({backyard_rule()}, kProperty, motion, {ArmMode::kArmedAway, 12 * 60}).empty());
}

TEST(Rules, ZoneAndEventTypeMustMatch) {
  // node-12 is in the hallway, not the backyard.
  EXPECT_TRUE(evaluate_rules({backyard_rule()}, kProperty, testing::motion("node-12", 1, 1),
                             {ArmMode::kArmedAway, k2am})
                  .empty());
  EXPECT_TRUE(evaluate_rules({backyard_rule()}, kProperty,
                             testing::motion("node-07", 1, 1, /*detected=*/false),
                             {ArmMode::kArmedAway, k2am})
                  .empty());
}

TEST(Rules, DisabledRuleOrOtherPropertyNeverFires) {
  Rule off = backyard_rule();
  off.enabled = false;
  auto motion = testing::motion("node-07", 1, 1);
  EXPECT_TRUE(evaluate_rules({off}, kProperty, motion, {ArmMode::kArmedAway, k2am}).empty());
  Rule other = backyard_rule();
  other.property = PropertyId("house-02");
  EXPECT_TRUE(evaluate_rules({other}, kProperty, motion, {ArmMode::kArmedAway, k2am}).empty());
}

TEST(Rules, ReEvaluatingSameEventYieldsSameIntentKey) {
  auto motion = testing::motion("node-07", 1, 1);
  auto a = evaluate_rules({backyard_rule()}, kProperty, motion, {ArmMode::kArmedAway, k2am});
  auto b = evaluate_rules({backyard_rule()}, kProperty, motion, {ArmMode::kArmedAway, k2am});
  ASSERT_EQ(a.size(), 1u);
  EXPECT_EQ(a[0].key, b[0].key);
  // A new rule revision is a distinct decision.
  Rule rev4 = backyard_rule();
  rev4.revision = 4;
  EXPECT_NE(evaluate_rules({rev4}, kProperty, motion, {ArmMode::kArmedAway, k2am})[0].key,
            a[0].key);
}

TEST(TimeWindow, HandlesWrapAndWholeDay) {
  TimeWindow night{22 * 60, 6 * 60};
  EXPECT_TRUE(night.contains(23 * 60));
  EXPECT_TRUE(night.contains(0));
  EXPECT_FALSE(night.contains(6 * 60));
  EXPECT_FALSE(night.contains(12 * 60));
  TimeWindow day{9 * 60, 17 * 60};
  EXPECT_TRUE(day.contains(9 * 60));
  EXPECT_FALSE(day.contains(17 * 60));
  EXPECT_TRUE((TimeWindow{0, 0}).contains(777));
}

TEST(Rules, ToCommandCarriesTtlCausationAndIdempotencyKey) {
  auto intent = evaluate_rules({backyard_rule()}, kProperty, testing::motion("node-07", 1, 1),
                               {ArmMode::kArmedAway, k2am})[0];
  Command c = to_command(intent, Timestamp{1'000});
  EXPECT_EQ(c.idempotency_key, intent.key);
  EXPECT_EQ(c.expires_at, Timestamp{6'000});
  EXPECT_EQ(c.causation, EventId("node-07:1:1"));
  EXPECT_EQ(c.principal, PrincipalId("rule:backyard_motion_warning"));
}

}  // namespace
}  // namespace talos
