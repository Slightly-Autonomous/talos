// TL-SN-002 tamper causes, radio power cap, reset report and sleep profiles
// (owner 2026-11-22, HD-043/045/049) on the virtual board.
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "firmware/esp_idf/main/board_tl_sn_002.h"
#include "firmware/sim/virtual_board.h"

namespace talos::fw::sim {
namespace {

constexpr std::uint8_t kMag = 0, kVib = 1;

Node sn002() { return Node(tl_sn_002_board()); }

std::vector<DeviceEvent> run_acked(Node& n, TimeMs ms) {
  std::vector<DeviceEvent> out;
  for (TimeMs t = 0; t < ms; t += 1'000) {
    n.run_for(std::min<TimeMs>(1'000, ms - t));
    for (const auto& e : n.drain_and_ack()) out.push_back(e);
  }
  return out;
}

std::vector<DeviceEvent> tampers(const std::vector<DeviceEvent>& ev, TamperCause c) {
  std::vector<DeviceEvent> out;
  for (const auto& e : ev) {
    if (e.kind == EventKind::kTamper && e.a == static_cast<std::int32_t>(c)) out.push_back(e);
  }
  return out;
}

void settle(Node& n, bool usb = false, std::uint32_t mv = 3'000) {
  n.sn002().usb = usb;
  n.adc.mv = mv;
  n.hall(kMag).set_field(30'000, 0, 0);  // door closed
  n.boot();
  run_acked(n, PirDriver::kPir223d.warm_min_ms + PirDriver::kPir223d.quiet_ms + 100);
  n.drain_and_ack();
  n.run_for(10);
}

constexpr TimeMs kDebounce = TamperDebouncer::kDebounceMs + 20;

// --- battery door vs lid ---------------------------------------------------------------------

TEST(Sn002BatteryDoor, OpeningTheDoorIsItsOwnTamperNotTheLid) {
  Node n = sn002();
  settle(n);
  n.sn002().battery_door_open = true;
  n.run_for(kDebounce);
  auto ev = n.drain_and_ack();
  const auto door = tampers(ev, TamperCause::kBatteryDoor);
  ASSERT_EQ(door.size(), 1u);
  EXPECT_TRUE(door[0].flag);
  EXPECT_EQ(door[0].priority, Priority::kP0Critical);
  EXPECT_EQ(door[0].b, 0u);
  EXPECT_TRUE(tampers(ev, TamperCause::kEnclosure).empty());
  EXPECT_TRUE(n.runtime().battery_door_open_reported());
  EXPECT_FALSE(n.runtime().lid_open_reported());

  n.sn002().battery_door_open = false;
  n.run_for(kDebounce);
  ev = n.drain_and_ack();
  const auto shut = tampers(ev, TamperCause::kBatteryDoor);
  ASSERT_EQ(shut.size(), 1u);
  EXPECT_FALSE(shut[0].flag);
  EXPECT_EQ(shut[0].priority, Priority::kP1State);
  EXPECT_TRUE(tampers(ev, TamperCause::kEnclosure).empty());
}

TEST(Sn002BatteryDoor, LidAndDoorTogetherEachReportAndBothCloseWithTheLoop) {
  Node n = sn002();
  settle(n);
  n.sn002().lid_open = true;
  n.run_for(kDebounce);
  auto ev = n.drain_and_ack();
  ASSERT_EQ(tampers(ev, TamperCause::kEnclosure).size(), 1u);
  // The door opens too while the lid is off: polled while the loop is open.
  n.sn002().battery_door_open = true;
  n.run_for(NodeRuntime::kMaskedPollMs + 50);
  ev = n.drain_and_ack();
  ASSERT_EQ(tampers(ev, TamperCause::kBatteryDoor).size(), 1u);
  EXPECT_TRUE(tampers(ev, TamperCause::kEnclosure).empty());  // still open, unobservable
  // Everything shut: the loop closes and both report closed.
  n.sn002().lid_open = false;
  n.sn002().battery_door_open = false;
  n.run_for(kDebounce);
  ev = n.drain_and_ack();
  const auto lid = tampers(ev, TamperCause::kEnclosure);
  const auto door = tampers(ev, TamperCause::kBatteryDoor);
  ASSERT_EQ(lid.size(), 1u);
  ASSERT_EQ(door.size(), 1u);
  EXPECT_FALSE(lid[0].flag);
  EXPECT_FALSE(door[0].flag);
}

TEST(Sn002BatteryDoor, DoorOpenUnlocksTheConsoleBecauseItAlarms) {
  // Any open loop is the P0 alarm, so the console unlocks for whoever opened it.
  Node n = sn002();
  settle(n, true);
  EXPECT_TRUE(n.runtime().console_locked());
  n.sn002().battery_door_open = true;
  n.run_for(kDebounce);
  EXPECT_FALSE(n.runtime().console_locked());
}

// --- magnet defeat ---------------------------------------------------------------------------

TEST(Sn002MagnetDefeat, AStrongForeignMagnetIsCaughtAtTheNextHeartbeat) {
  Node n = sn002();
  settle(n);
  // Attacker holds a big magnet at the sensor: the door axis only rises, so the
  // closed-door interrupt (a falling field) never fires.
  n.hall(kMag).set_field(110'000, 0, 0);
  auto ev = run_acked(n, 60'000 + 100);
  auto t = tampers(ev, TamperCause::kMagnetDefeat);
  ASSERT_EQ(t.size(), 1u);
  EXPECT_TRUE(t[0].flag);
  EXPECT_EQ(t[0].slot, kMag);
  EXPECT_EQ(t[0].priority, Priority::kP0Critical);
  EXPECT_TRUE(n.runtime().magnet_defeated());
  // Magnet taken away, door still shut: restored.
  n.hall(kMag).set_field(30'000, 0, 0);
  ev = run_acked(n, 60'000 + 100);
  t = tampers(ev, TamperCause::kMagnetDefeat);
  ASSERT_EQ(t.size(), 1u);
  EXPECT_FALSE(t[0].flag);
  EXPECT_FALSE(n.runtime().magnet_defeated());
}

TEST(Sn002MagnetDefeat, ASubstituteMagnetAtAnotherAngleIsCaughtAtOnce) {
  Node n = sn002();
  settle(n);
  // Same strength, other axis: the door axis falls (interrupt), the field stays strong.
  n.hall(kMag).set_field(0, 30'000, 0);
  const auto ev = run_acked(n, 1'000);
  const auto t = tampers(ev, TamperCause::kMagnetDefeat);
  ASSERT_EQ(t.size(), 1u);
  EXPECT_TRUE(t[0].flag);
}

TEST(Sn002MagnetDefeat, NormalDoorUseNeverLooksLikeDefeat) {
  Node n = sn002();
  settle(n);
  for (int i = 0; i < 3; ++i) {
    n.hall(kMag).set_field(1'000, 0, 0);  // open
    run_acked(n, 2'000);
    n.hall(kMag).set_field(28'000 + 2'000 * i, 500, 0);  // closed, a little differently each time
    run_acked(n, 2'000);
  }
  const auto ev = run_acked(n, 60'000 + 100);
  EXPECT_TRUE(tampers(ev, TamperCause::kMagnetDefeat).empty());
  EXPECT_FALSE(n.runtime().magnet_defeated());
}

// --- mount pose -----------------------------------------------------------------------------

TEST(Sn002Mount, ClosingTheLidAfterInstallCapturesThePoseAndPryingItOffAlarms) {
  Node n = sn002();
  settle(n);
  EXPECT_FALSE(n.runtime().mount_baseline().has_value());  // never installed
  n.imu(kVib).static_mg = {0, -1'000, 0};                  // on the frame, upright
  n.sn002().lid_open = true;
  n.run_for(kDebounce);
  n.sn002().lid_open = false;
  run_acked(n, NodeRuntime::kMountSettleMs + 1'000);
  ASSERT_TRUE(n.runtime().mount_baseline().has_value());
  EXPECT_NEAR(n.runtime().mount_baseline()->y, -1'000, 5);
  // Pried off the frame: a jolt and a new pose.
  n.imu(kVib).static_mg = {0, -500, 866};
  n.imu(kVib).impact(1'500);
  const auto ev = run_acked(n, NodeRuntime::kMountAfterImpactMs + 1'000);
  ASSERT_FALSE(std::none_of(ev.begin(), ev.end(),
                            [](const DeviceEvent& e) { return e.kind == EventKind::kImpact; }));
  const auto t = tampers(ev, TamperCause::kMountMoved);
  ASSERT_EQ(t.size(), 1u);
  EXPECT_TRUE(t[0].flag);
  EXPECT_EQ(t[0].slot, kVib);
  EXPECT_TRUE(n.runtime().mount_moved());
}

TEST(Sn002Mount, ThePoseSurvivesAReboot) {
  Node n = sn002();
  settle(n);
  n.imu(kVib).static_mg = {0, -1'000, 0};
  n.sn002().lid_open = true;
  n.run_for(kDebounce);
  n.sn002().lid_open = false;
  run_acked(n, NodeRuntime::kMountSettleMs + 1'000);
  ASSERT_TRUE(n.runtime().mount_baseline().has_value());
  n.boot();
  ASSERT_TRUE(n.runtime().mount_baseline().has_value());
  // A tilt seen at a heartbeat (no impact needed) still alarms.
  n.imu(kVib).static_mg = {700, -700, 0};
  const auto ev = run_acked(n, 60'000 + 100);
  EXPECT_EQ(tampers(ev, TamperCause::kMountMoved).size(), 1u);
}

// --- TX power cap ---------------------------------------------------------------------------

TEST(Sn002TxPower, FullPowerOnHealthyCellsCappedWhenLowAndOnUsbAgain) {
  Node n = sn002();
  settle(n, false, 2'900);
  EXPECT_EQ(n.link.tx_power_dbm, 18);
  EXPECT_FALSE(n.runtime().tx_capped());
  n.adc.mv = 2'150;  // loaded reading under the 2.2 V low-battery threshold
  run_acked(n, 60'000 + 100);
  EXPECT_TRUE(n.runtime().tx_capped());
  EXPECT_EQ(n.link.tx_power_dbm, 10);
  n.sn002().usb = true;
  run_acked(n, 60'000 + 100);
  EXPECT_FALSE(n.runtime().tx_capped());
  EXPECT_EQ(n.link.tx_power_dbm, 18);
}

// --- reset report ---------------------------------------------------------------------------

TEST(Sn002Reset, ColdBootReportsItsCauseDeepSleepWakesDoNot) {
  Node n = sn002();
  n.hall(kMag).set_field(30'000, 0, 0);
  n.sleep.info.cause = ResetCause::kUsb;  // reset through the outside USB-C port
  n.boot();
  n.run_for(100);
  auto ev = n.drain_and_ack();
  std::vector<DeviceEvent> r;
  std::copy_if(ev.begin(), ev.end(), std::back_inserter(r),
               [](const DeviceEvent& e) { return e.kind == EventKind::kReset; });
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].a, static_cast<std::int32_t>(ResetCause::kUsb));
  EXPECT_EQ(r[0].b, 1u);  // with the enclosure shut
  EXPECT_EQ(r[0].priority, Priority::kP1State);
  run_acked(n, 2'000);
  n.wake_from_deep_sleep(1u << board::sn002::kUserButton);
  n.run_for(100);
  ev = n.drain_and_ack();
  EXPECT_TRUE(std::none_of(ev.begin(), ev.end(),
                           [](const DeviceEvent& e) { return e.kind == EventKind::kReset; }));
}

// --- sleep profiles -------------------------------------------------------------------------

TEST(Sn002Sleep, DeepSleepByDefaultOnBatteryAwakeOnUsb) {
  Node n = sn002();
  settle(n);
  EXPECT_EQ(n.runtime().power_profile(), PowerProfile::kDeepSleep);
  EXPECT_EQ(n.runtime().sleep_plan().kind, SleepPlan::Kind::kDeep);
  EXPECT_EQ(n.runtime().sleep_plan(true).kind, SleepPlan::Kind::kStayAwake);  // console in use
  Node u = sn002();
  settle(u, true);
  EXPECT_EQ(u.runtime().sleep_plan().kind, SleepPlan::Kind::kStayAwake);
}

TEST(Sn002Sleep, TheHubCanAskForInstantOrSleepyChildPerNode) {
  Node n = sn002();
  settle(n);
  ConfigUpdate c{7, 0, 0, 0};
  c.power_profile = static_cast<std::uint8_t>(PowerProfile::kAlwaysOn);
  n.link.downlink.push_back(c);
  run_acked(n, 1'000);
  EXPECT_EQ(n.runtime().power_profile(), PowerProfile::kAlwaysOn);
  EXPECT_EQ(n.runtime().sleep_plan().kind, SleepPlan::Kind::kStayAwake);
  c.revision = 8;
  c.power_profile = static_cast<std::uint8_t>(PowerProfile::kSleepyChild);
  n.link.downlink.push_back(c);
  run_acked(n, 1'000);
  n.run_for(10);
  EXPECT_EQ(n.runtime().sleep_plan().kind, SleepPlan::Kind::kLight);
  // Persisted: a reboot keeps the owner's choice.
  n.boot();
  EXPECT_EQ(n.runtime().power_profile(), PowerProfile::kSleepyChild);
  // An out-of-range value is ignored, the rest of the config applies.
  c.revision = 9;
  c.power_profile = 7;
  c.impact_threshold_mg = 800;
  n.link.downlink.push_back(c);
  run_acked(n, 1'000);
  EXPECT_EQ(n.runtime().power_profile(), PowerProfile::kSleepyChild);
  EXPECT_EQ(n.runtime().config().impact_threshold_mg, 800u);
}

TEST(Sn002Sleep, AMagnetDefeatStateSurvivesADeepSleepWake) {
  Node n = sn002();
  settle(n);
  n.hall(kMag).set_field(110'000, 0, 0);
  run_acked(n, 60'000 + 100);
  ASSERT_TRUE(n.runtime().magnet_defeated());
  n.wake_from_deep_sleep(0);  // timer wake
  EXPECT_TRUE(n.runtime().magnet_defeated());
  // No second alarm for the same defeat after the wake.
  const auto ev = run_acked(n, 60'000 + 100);
  EXPECT_TRUE(tampers(ev, TamperCause::kMagnetDefeat).empty());
}

}  // namespace
}  // namespace talos::fw::sim
