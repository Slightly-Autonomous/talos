// MagnetDefeat, MountPose (tamper_detect.h) and SleepPolicy (sleep_policy.h).
#include <gtest/gtest.h>

#include "firmware/portable/sleep_policy.h"
#include "firmware/portable/tamper_detect.h"

namespace talos::fw {
namespace {

TEST(MagnetDefeat, NoBaselineJudgesNothing) {
  MagnetDefeat m;
  EXPECT_EQ(m.check({200'000, 0, 0}), std::nullopt);
}

TEST(MagnetDefeat, DoorMagnetJitterIsNotDefeat) {
  MagnetDefeat m;
  m.set_baseline({0, 0, 25'000});
  EXPECT_EQ(m.check({1'000, -800, 26'500}), std::nullopt);
  EXPECT_EQ(m.check({0, 0, 40'000}), std::nullopt);  // door pushed in harder: under 3x
  EXPECT_FALSE(m.defeated());
}

TEST(MagnetDefeat, StrongForeignMagnetIsDefeat) {
  MagnetDefeat m;
  m.set_baseline({0, 0, 25'000});
  EXPECT_EQ(m.check({0, 0, 90'000}), std::optional<bool>(true));   // > 3x the door magnet
  EXPECT_EQ(m.check({0, 0, 95'000}), std::nullopt);                // still defeated, no repeat
  EXPECT_EQ(m.check({0, 0, 30'000}), std::optional<bool>(false));  // back near the baseline
}

TEST(MagnetDefeat, CeilingAppliesEvenWithAStrongBaseline) {
  MagnetDefeat m;
  m.set_baseline({0, 0, 60'000});
  EXPECT_EQ(m.check({0, 0, 120'000}), std::optional<bool>(true));
}

TEST(MagnetDefeat, ForeignMagnetAtTheSameStrengthButAnotherAngle) {
  MagnetDefeat m;
  m.set_baseline({0, 0, 25'000});
  // A substitute magnet tuned to the same |B| but placed beside the sensor.
  EXPECT_EQ(m.check({20'000, 0, 15'000}), std::optional<bool>(true));  // ~53 degrees
}

TEST(MagnetDefeat, ReversedPolarityIsDefeat) {
  MagnetDefeat m;
  m.set_baseline({0, 0, 25'000});
  EXPECT_EQ(m.check({0, 0, -25'000}), std::optional<bool>(true));
}

TEST(MagnetDefeat, HysteresisHoldsBetweenThresholds) {
  MagnetDefeat m;
  m.set_baseline({0, 0, 20'000});
  ASSERT_EQ(m.check({0, 0, 70'000}), std::optional<bool>(true));
  EXPECT_EQ(m.check({0, 0, 35'000}), std::nullopt);  // 1.75x: below 3x but above 1.5x
  EXPECT_EQ(m.check({0, 0, 28'000}), std::optional<bool>(false));
}

TEST(MagnetDefeat, RestoreKeepsStateAcrossAWake) {
  MagnetDefeat m;
  m.restore(Vec3{0, 0, 25'000}, true);
  EXPECT_TRUE(m.defeated());
  MagnetDefeat none;
  none.restore(std::nullopt, true);  // no baseline: defeat can't stand
  EXPECT_FALSE(none.defeated());
}

TEST(MountPose, SmallTiltIsNotMovement) {
  MountPose p;
  p.set_baseline({0, -1'000, 0});
  EXPECT_EQ(p.check({170, -985, 0}), std::nullopt);  // ~10 degrees
}

TEST(MountPose, PryingOffTheFrameIsMovement) {
  MountPose p;
  p.set_baseline({0, -1'000, 0});
  EXPECT_EQ(p.check({0, -500, 866}), std::optional<bool>(true));  // 60 degrees
  EXPECT_EQ(p.check({0, -1'000, 30}), std::optional<bool>(false));
}

TEST(MountPose, ImplausibleReadingsAreIgnored) {
  MountPose p;
  p.set_baseline({0, -1'000, 0});
  EXPECT_EQ(p.check({0, 0, 300}), std::nullopt);    // free fall / carried
  EXPECT_EQ(p.check({2'000, 0, 0}), std::nullopt);  // shaken
  EXPECT_FALSE(p.moved());
}

SleepInputs base() {
  SleepInputs in;
  in.now_ms = 1'000;
  in.next_wake_ms = 61'000;
  return in;
}

TEST(SleepPolicy, DeepSleepByDefaultForLongGaps) {
  EXPECT_EQ(SleepPolicy::plan(base()).kind, SleepPlan::Kind::kDeep);
  EXPECT_EQ(SleepPolicy::plan(base()).until_ms, 61'000u);
}

TEST(SleepPolicy, UsbPowerAndAlwaysOnNeverSleep) {
  auto in = base();
  in.external_power = true;
  EXPECT_EQ(SleepPolicy::plan(in).kind, SleepPlan::Kind::kStayAwake);
  in = base();
  in.profile = PowerProfile::kAlwaysOn;
  EXPECT_EQ(SleepPolicy::plan(in).kind, SleepPlan::Kind::kStayAwake);
}

TEST(SleepPolicy, SleepyChildOnlyLightSleeps) {
  auto in = base();
  in.profile = PowerProfile::kSleepyChild;
  EXPECT_EQ(SleepPolicy::plan(in).kind, SleepPlan::Kind::kLight);
}

TEST(SleepPolicy, PendingSendsAndShortGapsUseLightSleep) {
  auto in = base();
  in.sends_pending = true;
  EXPECT_EQ(SleepPolicy::plan(in).kind, SleepPlan::Kind::kLight);
  in = base();
  in.next_wake_ms = in.now_ms + 2'000;
  EXPECT_EQ(SleepPolicy::plan(in).kind, SleepPlan::Kind::kLight);
}

TEST(SleepPolicy, BusyConsoleOrTinyGapsStayAwake) {
  auto in = base();
  in.busy = true;
  EXPECT_EQ(SleepPolicy::plan(in).kind, SleepPlan::Kind::kStayAwake);
  in = base();
  in.console_active = true;
  EXPECT_EQ(SleepPolicy::plan(in).kind, SleepPlan::Kind::kStayAwake);
  in = base();
  in.next_wake_ms = in.now_ms + 2;
  EXPECT_EQ(SleepPolicy::plan(in).kind, SleepPlan::Kind::kStayAwake);
}

}  // namespace
}  // namespace talos::fw
