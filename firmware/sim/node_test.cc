// Portable firmware on the virtual board (Phase 5 exit criteria).
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>

#include "common/sha256/sha256.h"
#include "firmware/sim/virtual_board.h"

namespace talos::fw::sim {
namespace {

std::vector<DeviceEvent> of_kind(const std::vector<DeviceEvent>& v, EventKind k) {
  std::vector<DeviceEvent> out;
  std::copy_if(v.begin(), v.end(), std::back_inserter(out),
               [&](const DeviceEvent& e) { return e.kind == k; });
  return out;
}

// A PIR reports nothing until its warm-up has passed with the output at rest.
void warm_up(Node& n) {
  n.run_for(PirDriver::kWarmMinMs + PirDriver::kQuietMs);
  n.drain_and_ack();
}

Node four_slot(PowerSource src = PowerSource::kUsb) {
  Node n(default_board());
  n.power.source_ = src;
  n.insert(0, ModuleCode::kPirEkmb);
  n.insert(1, ModuleCode::kLis2dw12);
  n.insert(2, ModuleCode::kSmartCameraS3);
  n.insert(3, ModuleCode::kSht40);
  return n;
}

// --- Discovery and resource checks ------------------------------------------------------------

TEST(Discovery, FourSlotPirImuCameraEnvOnUsb) {
  Node n = four_slot();
  n.boot();
  for (std::uint8_t i = 0; i < 4; ++i) EXPECT_TRUE(n.runtime().slot(i).enabled) << int(i);
  EXPECT_TRUE(n.power.rail(2, Rail::kSwitched5v));
  n.run_for(10);
  EXPECT_EQ(of_kind(n.drain_and_ack(), EventKind::kModulePresent).size(), 4u);
}

TEST(Discovery, SecondCombinationContactsPirEnv) {
  Node n(default_board());
  n.insert(0, ModuleCode::kReedContact);
  n.insert(1, ModuleCode::kReedContact);
  n.insert(2, ModuleCode::kPirEkmb);
  n.insert(3, ModuleCode::kSht40);
  n.boot();
  for (std::uint8_t i = 0; i < 4; ++i) EXPECT_TRUE(n.runtime().slot(i).enabled) << int(i);
  n.run_for(10);
  auto events = n.drain_and_ack();
  EXPECT_EQ(of_kind(events, EventKind::kContact).size(), 2u);  // boot state reports
}

TEST(Discovery, CameraDisabledOnBatteryButCriticalSensingStays) {
  Node n = four_slot(PowerSource::kBatteryOnly);
  n.boot();
  EXPECT_TRUE(n.runtime().slot(0).enabled);
  EXPECT_TRUE(n.runtime().slot(1).enabled);
  EXPECT_FALSE(n.runtime().slot(2).enabled);
  EXPECT_EQ(n.runtime().slot(2).fault, ModuleFault::kDisabledForPower);
  EXPECT_FALSE(n.power.rail(2, Rail::kSwitched5v));
}

TEST(Discovery, UnknownEmptyMissingResourceAndProbeFailure) {
  BoardSupport b = default_board();
  b.slots[1].wake_line = 0xFF;  // this board revision has no wake line on slot 1
  Node n(b);
  n.insert(0, static_cast<ModuleCode>(0x7777));  // unknown code
  n.insert(1, ModuleCode::kPirEkmb);             // needs wake: missing
  n.insert(2, ModuleCode::kLis2dw12);            // WHO_AM_I will be wrong
  n.imu(2).regs[0x0F] = 0x00;
  // slot 3 empty
  n.boot();
  EXPECT_EQ(n.runtime().slot(0).fault, ModuleFault::kUnknownModule);
  EXPECT_EQ(n.runtime().slot(1).fault, ModuleFault::kMissingResource);
  EXPECT_EQ(n.runtime().slot(2).fault, ModuleFault::kInitFailed);
  EXPECT_EQ(n.runtime().slot(3).code, ModuleCode::kNone);
  EXPECT_EQ(n.runtime().slot(3).fault, ModuleFault::kNone);
  for (std::uint8_t i = 0; i < 4; ++i) EXPECT_FALSE(n.runtime().slot(i).enabled);
}

TEST(Discovery, BrokenI2cBranchIsolatedFromOtherSlots) {
  Node n = four_slot();
  n.i2c.break_branch(3, true);  // SHT40 branch shorted
  n.boot();
  EXPECT_EQ(n.runtime().slot(3).code, ModuleCode::kNone);  // unreadable: treated as empty
  EXPECT_TRUE(n.runtime().slot(0).enabled);
  EXPECT_TRUE(n.runtime().slot(1).enabled);
}

// --- Drivers ----------------------------------------------------------------------------------

TEST(Contact, BounceProducesOneDebouncedEvent) {
  Node n(default_board());
  n.insert(0, ModuleCode::kReedContact);
  n.boot();
  n.run_for(5);
  n.drain_and_ack();
  // Door opens: the reed chatters for 6 ms before settling open.
  for (int i = 0; i < 6; ++i) {
    n.gpio.set(0, i % 2 == 0);
    n.run_for(1);
  }
  n.gpio.set(0, true);
  n.run_for(100);
  auto contacts = of_kind(n.drain_and_ack(), EventKind::kContact);
  ASSERT_EQ(contacts.size(), 1u);
  EXPECT_TRUE(contacts[0].flag);
  EXPECT_EQ(contacts[0].priority, Priority::kP0Critical);
  // A glitch shorter than the debounce window is ignored.
  n.gpio.set(0, false);
  n.run_for(5);
  n.gpio.set(0, true);
  n.run_for(100);
  EXPECT_TRUE(of_kind(n.drain_and_ack(), EventKind::kContact).empty());
}

// TL-MOD-001 (TMAG5273): the same door works whichever axis the magnet lines up with.
class HallContact : public ::testing::TestWithParam<int> {};

TEST_P(HallContact, OpenCloseOnAnyAxisAndReArms) {
  const int axis = GetParam();
  auto field = [&](std::int32_t ut) {
    std::int32_t f[3] = {40, -25, 30};  // stray field, well under the thresholds
    f[axis] = ut;
    return std::array<std::int32_t, 3>{f[0], f[1], f[2]};
  };
  Node n(default_board());
  n.insert(0, ModuleCode::kTmag5273Contact);
  auto closed = field(axis == 1 ? -8'000 : 8'000);  // either magnet polarity
  n.hall(0).set_field(closed[0], closed[1], closed[2]);
  n.boot();
  n.run_for(5);
  auto boot = of_kind(n.drain_and_ack(), EventKind::kContact);
  ASSERT_EQ(boot.size(), 1u);
  EXPECT_FALSE(boot[0].flag);  // closed
  EXPECT_EQ(boot[0].b, 1u);    // state report, not a transition
  EXPECT_TRUE(n.hall(0).in_wake_sleep());
  EXPECT_FALSE(n.gpio.read(0));

  // A small wobble while closed raises nothing.
  auto wobble = field(axis == 1 ? -6'000 : 6'000);
  n.hall(0).set_field(wobble[0], wobble[1], wobble[2]);
  n.run_for(10);
  EXPECT_TRUE(of_kind(n.drain_and_ack(), EventKind::kContact).empty());

  // Door opens: the field collapses.
  auto open = field(200);
  n.hall(0).set_field(open[0], open[1], open[2]);
  n.run_for(10);
  auto opened = of_kind(n.drain_and_ack(), EventKind::kContact);
  ASSERT_EQ(opened.size(), 1u);
  EXPECT_TRUE(opened[0].flag);
  EXPECT_EQ(opened[0].priority, Priority::kP0Critical);
  EXPECT_FALSE(n.gpio.read(0));            // latch cleared by the read
  EXPECT_TRUE(n.hall(0).in_wake_sleep());  // re-armed, not left in 0.45 mA standby

  // Door closes again.
  n.hall(0).set_field(closed[0], closed[1], closed[2]);
  n.run_for(10);
  auto shut = of_kind(n.drain_and_ack(), EventKind::kContact);
  ASSERT_EQ(shut.size(), 1u);
  EXPECT_FALSE(shut[0].flag);
  EXPECT_TRUE(n.hall(0).in_wake_sleep());
}

INSTANTIATE_TEST_SUITE_P(Axes, HallContact, ::testing::Values(0, 1, 2));

TEST(HallContactCodes, ThresholdCodesAndBootOpen) {
  EXPECT_EQ(Tmag5273ContactDriver::threshold_code(1'500), 5);  // 5 x 0.3125 mT = 1.56 mT
  EXPECT_EQ(Tmag5273ContactDriver::threshold_code(3'000), 10);
  EXPECT_EQ(Tmag5273ContactDriver::threshold_code(0), 1);
  EXPECT_EQ(Tmag5273ContactDriver::threshold_code(100'000), 127);
  Node n(default_board());
  n.insert(0, ModuleCode::kTmag5273Contact);  // no magnet anywhere
  n.boot();
  n.run_for(5);
  auto boot = of_kind(n.drain_and_ack(), EventKind::kContact);
  ASSERT_EQ(boot.size(), 1u);
  EXPECT_TRUE(boot[0].flag);
}

TEST(Pir, RisingAndFallingEdgesAfterWarmUp) {
  Node n(default_board());
  n.insert(0, ModuleCode::kPirEkmb);
  n.boot();
  warm_up(n);
  n.gpio.set(0, true);
  n.run_for(100);
  n.gpio.set(0, false);
  n.run_for(10);
  auto m = of_kind(n.drain_and_ack(), EventKind::kMotion);
  ASSERT_EQ(m.size(), 2u);
  EXPECT_TRUE(m[0].flag);
  EXPECT_EQ(m[0].priority, Priority::kP0Critical);
  EXPECT_FALSE(m[1].flag);
}

TEST(Pir, WarmUpChatterIsNeverMotion) {
  Node n(default_board());
  n.insert(0, ModuleCode::kPirEkmb);
  n.boot();
  // The EKMB output is undefined for up to 210 s after power-up.
  for (int i = 0; i < 10; ++i) {
    n.gpio.set(0, i % 2 == 0);
    n.run_for(2'000);
  }
  n.gpio.set(0, false);
  n.run_for(100);
  EXPECT_TRUE(of_kind(n.drain_and_ack(), EventKind::kMotion).empty());
  EXPECT_TRUE(std::get<PirDriver>(n.runtime().slot(0).driver).warming());
  // Settled: 30 s since power-up and 3 s at rest.
  n.run_for(PirDriver::kWarmMinMs);
  EXPECT_FALSE(std::get<PirDriver>(n.runtime().slot(0).driver).warming());
}

TEST(Pir, WarmUpThatNeverSettlesIsAFault) {
  Node n(default_board());
  n.insert(0, ModuleCode::kPirEkmb);
  n.boot();
  n.gpio.set(0, true);  // e.g. OUT and GND swapped: the output sits high
  n.run_for(PirDriver::kWarmMaxMs + 1'000);
  auto events = n.drain_and_ack();
  EXPECT_TRUE(of_kind(events, EventKind::kMotion).empty());
  auto faults = of_kind(events, EventKind::kModuleFault);
  ASSERT_EQ(faults.size(), 1u);
  EXPECT_EQ(faults[0].a, static_cast<std::int32_t>(ModuleFault::kWarmupTimeout));
}

TEST(Pir, SpikeShorterThanGlitchFilterIsIgnored) {
  Node n(default_board());
  n.insert(0, ModuleCode::kPirEkmb);
  n.boot();
  warm_up(n);
  n.gpio.set(0, true);
  n.run_for(PirDriver::kGlitchMs - 10);
  n.gpio.set(0, false);
  n.run_for(100);
  EXPECT_TRUE(of_kind(n.drain_and_ack(), EventKind::kMotion).empty());
  EXPECT_EQ(std::get<PirDriver>(n.runtime().slot(0).driver).glitches(), 1u);
}

TEST(Imu, ImpactAboveThresholdReportsPeakAndClearsInterrupt) {
  Node n = four_slot();
  n.boot();
  EXPECT_EQ(n.imu(1).regs[0x34], Lis2dw12Driver::threshold_register(600));
  n.imu(1).impact(400);  // below 600 mg: no wake
  n.run_for(10);
  EXPECT_TRUE(of_kind(n.drain_and_ack(), EventKind::kImpact).empty());
  n.imu(1).impact(900);
  n.run_for(10);
  auto imp = of_kind(n.drain_and_ack(), EventKind::kImpact);
  ASSERT_EQ(imp.size(), 1u);
  EXPECT_NEAR(imp[0].a, 900, 5);
  EXPECT_FALSE(n.gpio.read(1));  // INT1 released after WAKE_UP_SRC read
}

TEST(Imu, ConfigUpdateRetunesThresholdPersistsAndIgnoresStale) {
  Node n = four_slot();
  n.boot();
  n.link.downlink.push_back(ConfigUpdate{5, 0, 0, 300});
  n.run_for(5);
  EXPECT_EQ(n.runtime().config().revision, 5u);
  EXPECT_EQ(n.imu(1).regs[0x34], Lis2dw12Driver::threshold_register(300));
  auto applied = of_kind(n.drain_and_ack(), EventKind::kConfigApplied);
  ASSERT_EQ(applied.size(), 1u);
  EXPECT_EQ(applied[0].a, 5);
  n.link.downlink.push_back(ConfigUpdate{4, 0, 0, 900});  // stale revision
  n.run_for(5);
  EXPECT_EQ(n.runtime().config().impact_threshold_mg, 300u);
  n.boot();  // survives reboot
  EXPECT_EQ(n.runtime().config().revision, 5u);
  EXPECT_EQ(n.imu(1).regs[0x34], Lis2dw12Driver::threshold_register(300));
}

TEST(Sht40, ReadsValuesAndReportsCrcFaults) {
  Node n = four_slot();
  n.sht(3).temperature_c = 23.4;
  n.sht(3).humidity_pct = 51.0;
  n.boot();
  n.run_for(Sht40Driver::kMeasureMs + 2);
  auto env = of_kind(n.drain_and_ack(), EventKind::kEnvironment);
  ASSERT_EQ(env.size(), 1u);
  EXPECT_NEAR(env[0].a, 23'400, 10);
  EXPECT_NEAR(env[0].b, 510u, 2u);
  n.sht(3).corrupt_crc = true;
  n.run_for(300'000);
  auto faults = of_kind(n.drain_and_ack(), EventKind::kModuleFault);
  ASSERT_FALSE(faults.empty());
  EXPECT_EQ(faults.back().a, static_cast<std::int32_t>(ModuleFault::kBusError));
}

// --- Hardware-faithful driver behaviour (bench-test-plan.md FW-B1..B5, B10, B11) ---------------

TEST(Sht40, WaitsOutTheConversionAndReportsEachFault) {
  Node n(default_board());
  n.insert(0, ModuleCode::kSht40);
  n.sht(0).measure_ms = 8;  // the read header NACKs for 8 ms after 0xFD
  n.boot();
  EXPECT_EQ(std::get<Sht40Driver>(n.runtime().slot(0).driver).serial(), 0x1234ABCDu);
  n.run_for(Sht40Driver::kMeasureMs + 1);
  EXPECT_EQ(of_kind(n.drain_and_ack(), EventKind::kEnvironment).size(), 1u);
  auto fault_detail = [&](auto setup) {
    Node m(default_board());
    m.insert(0, ModuleCode::kSht40);
    m.boot();
    m.run_for(Sht40Driver::kMeasureMs + 1);
    m.drain_and_ack();
    setup(m.sht(0));
    m.run_for(300'100);
    auto f = of_kind(m.drain_and_ack(), EventKind::kModuleFault);
    return f.empty() ? 0u : f.back().b;
  };
  EXPECT_EQ(fault_detail([](Sht40& s) { s.corrupt_t_crc = true; }), 3u);
  EXPECT_EQ(fault_detail([](Sht40& s) { s.corrupt_crc = true; }), 4u);
  EXPECT_EQ(fault_detail([](Sht40& s) { s.measure_ms = 60; }), 2u);        // never answers in time
  EXPECT_EQ(fault_detail([](Sht40& s) { s.temperature_c = -45.0; }), 5u);  // raw 0: stuck output
}

TEST(Sht40, SlowButInSpecConversionStillReads) {
  Node n(default_board());
  n.insert(0, ModuleCode::kSht40);
  n.sht(0).measure_ms = 15;  // slower than the 9 ms wait, inside the retry grace
  n.boot();
  n.run_for(40);
  EXPECT_EQ(of_kind(n.drain_and_ack(), EventKind::kEnvironment).size(), 1u);
}

TEST(Sht40, MissingSensorBehindAWorkingIdIsAnInitFault) {
  Node n(default_board());
  n.insert(0, ModuleCode::kSht40);
  n.i2c.detach(0, Sht40Driver::kAddr);  // EEPROM answers, the sensor does not
  n.boot();
  EXPECT_EQ(n.runtime().slot(0).fault, ModuleFault::kInitFailed);
  EXPECT_FALSE(n.runtime().slot(0).enabled);
}

TEST(Imu, LatchedImpactSurvivesTheEspPollingLoop) {
  Node n(default_board());
  n.insert(1, ModuleCode::kLis2dw12);
  n.esp_polling = true;  // main.cc polls every <= 20 ms; the INT pulse is 10 ms at 100 Hz
  n.boot();
  EXPECT_EQ(n.imu(1).regs[Lis2dw12Driver::kCtrl7], Lis2dw12Driver::kCtrl7IntEnable);
  EXPECT_EQ(n.imu(1).regs[Lis2dw12Driver::kCtrl3] & Lis2dw12Driver::kCtrl3Lir,
            Lis2dw12Driver::kCtrl3Lir);
  n.run_for(25);
  n.drain_and_ack();
  for (int i = 0; i < 5; ++i) {
    n.imu(1).impact(900);
    n.run_for(500);
  }
  auto imp = of_kind(n.drain_and_ack(), EventKind::kImpact);
  ASSERT_EQ(imp.size(), 5u);
  EXPECT_NEAR(imp[0].a, 900, 5);  // high-pass path: no 1 g of gravity in the peak
}

TEST(Imu, StaleLatchedEventFromBeforeAResetIsCleared) {
  Node n(default_board());
  n.insert(1, ModuleCode::kLis2dw12);
  n.boot();
  n.imu(1).impact(900);  // event latched, then the MCU resets before reading it
  n.boot();
  n.run_for(20);
  EXPECT_TRUE(of_kind(n.drain_and_ack(), EventKind::kImpact).empty());
  EXPECT_FALSE(n.gpio.read(1));
}

TEST(Imu, WakeHeldHighWithoutAnEventIsAStuckInterrupt) {
  Node n(default_board());
  n.insert(1, ModuleCode::kLis2dw12);
  n.boot();
  n.imu(1).int_stuck_high = true;
  n.gpio.set(1, true);
  n.run_for(1'000);
  auto events = n.drain_and_ack();
  EXPECT_TRUE(of_kind(events, EventKind::kImpact).empty());
  auto faults = of_kind(events, EventKind::kModuleFault);
  ASSERT_EQ(faults.size(), 1u);
  EXPECT_EQ(faults[0].a, static_cast<std::int32_t>(ModuleFault::kStuckInterrupt));
}

TEST(Imu, RegisterThatDoesNotTakeWritesFailsInit) {
  Node n(default_board());
  n.insert(1, ModuleCode::kLis2dw12);
  n.imu(1).read_only_reg = Lis2dw12Driver::kCtrl7;
  n.boot();
  EXPECT_EQ(n.runtime().slot(1).fault, ModuleFault::kInitFailed);
}

TEST(HallContact, ObliqueMagnetOpeningIsSeenDespiteStaleAxes) {
  Node n(default_board());
  n.insert(0, ModuleCode::kTmag5273Contact);
  n.hall(0).set_field(6'000, 5'000, 0);  // magnet at an angle: X dominant, Y strong too
  n.boot();
  n.run_for(5);
  n.drain_and_ack();
  // Door opens. Only X is armed while closed, so Y's result register still
  // holds 5 mT from the last full conversion.
  n.hall(0).set_field(200, 150, 0);
  n.run_for(10);
  auto opened = of_kind(n.drain_and_ack(), EventKind::kContact);
  ASSERT_EQ(opened.size(), 1u);
  EXPECT_TRUE(opened[0].flag);
}

TEST(HallContact, SensorThatNeverConvertsFailsInitAndIsParkedInSleep) {
  Node n(default_board());
  n.insert(0, ModuleCode::kTmag5273Contact);
  n.hall(0).never_converts = true;
  n.boot();
  EXPECT_EQ(n.runtime().slot(0).fault, ModuleFault::kInitFailed);
  EXPECT_EQ(n.hall(0).mode(), Tmag5273ContactDriver::kModeSleep);  // 5 nA, not 0.45 mA standby
}

TEST(Discovery, BlankAndCorruptIdsAreFaultsNotEmptySlots) {
  Node n(default_board());
  n.insert(0, ModuleCode::kSht40);
  n.insert(1, ModuleCode::kSht40);
  n.id(0).regs[0] = n.id(0).regs[1] = 0xFF;  // never programmed
  n.id(1).regs[0] = n.id(1).regs[1] = 0x00;  // erased to zero
  n.boot();
  EXPECT_EQ(n.runtime().slot(0).fault, ModuleFault::kIdBlank);
  EXPECT_EQ(n.runtime().slot(1).fault, ModuleFault::kIdCorrupt);
  EXPECT_FALSE(n.runtime().slot(0).enabled);
  EXPECT_FALSE(n.runtime().slot(1).enabled);
}

TEST(Discovery, DeadMuxIsAFaultOnEverySlot) {
  Node n = four_slot();
  n.i2c.mux_nack = true;
  n.boot();
  for (std::uint8_t i = 0; i < 4; ++i) {
    EXPECT_EQ(n.runtime().slot(i).fault, ModuleFault::kMuxFault) << int(i);
  }
}

TEST(Discovery, MuxIsLeftDeselectedAfterEverySlotAccess) {
  Node n = four_slot();
  n.boot();
  EXPECT_EQ(n.i2c.mask(), 0);
  n.run_for(Sht40Driver::kMeasureMs + 2);
  EXPECT_EQ(n.i2c.mask(), 0);
  n.imu(1).impact(900);
  n.run_for(10);
  EXPECT_EQ(n.i2c.mask(), 0);
}

// --- Delivery -----------------------------------------------------------------------------------

TEST(Delivery, RetriesWithBackoffUntilAcked) {
  Node n(default_board());
  n.insert(0, ModuleCode::kPirEkmb);
  n.boot();
  warm_up(n);
  n.run_for(5);
  n.link.drop_next = 3;  // three transmissions lost in the air
  const auto before = n.link.attempts;
  n.gpio.set(0, true);
  n.run_for(1);
  n.run_for(500 + 1000 + 2000 + 50);  // backoff 0.5 s, 1 s, 2 s
  EXPECT_EQ(n.link.attempts - before, 4u);
  auto motion = of_kind(n.drain_and_ack(), EventKind::kMotion);
  ASSERT_EQ(motion.size(), 1u);
  n.run_for(20'000);
  EXPECT_TRUE(of_kind(n.drain_and_ack(), EventKind::kMotion).empty());  // acked: no resend
}

TEST(Delivery, QueuePressureDropsLowPriorityBeforeCritical) {
  EventQueue<4> q;
  auto ev = [](std::uint32_t seq, Priority p) {
    DeviceEvent e;
    e.seq = seq;
    e.priority = p;
    return e;
  };
  q.push(ev(1, Priority::kP2Health));
  q.push(ev(2, Priority::kP0Critical));
  q.push(ev(3, Priority::kP3Debug));
  q.push(ev(4, Priority::kP0Critical));
  q.push(ev(5, Priority::kP0Critical));  // evicts the P3
  q.push(ev(6, Priority::kP1State));     // evicts the P2
  EXPECT_EQ(q.dropped(), 2u);
  EXPECT_EQ(q.dropped_p0(), 0u);
  std::vector<std::uint32_t> kept;
  q.for_each([&](const DeviceEvent& e) { kept.push_back(e.seq); });
  std::sort(kept.begin(), kept.end());
  EXPECT_EQ(kept, (std::vector<std::uint32_t>{2, 4, 5, 6}));
}

TEST(Persistence, BootEpochIncrementsAndSequenceRestarts) {
  Node n(default_board());
  n.boot();
  const auto e1 = n.runtime().boot_epoch();
  n.boot();
  EXPECT_EQ(n.runtime().boot_epoch(), e1 + 1);
  n.run_for(5);
  auto events = n.drain_and_ack();
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.front().seq, 1u);
  EXPECT_EQ(events.front().boot_epoch, e1 + 1);
}

TEST(Persistence, UnackedCriticalEventSurvivesRebootWithItsIdentity) {
  Node n(default_board());
  n.insert(0, ModuleCode::kPirEkmb);
  n.boot();
  warm_up(n);
  n.run_for(5);
  n.link.connected = false;  // radio down during the intrusion
  n.gpio.set(0, true);
  n.run_for(50);
  const auto epoch = n.runtime().boot_epoch();
  n.boot();  // battery swap / brownout before delivery
  n.link.connected = true;
  n.run_for(50);
  auto motion = of_kind(n.drain_and_ack(), EventKind::kMotion);
  ASSERT_GE(motion.size(), 1u);
  EXPECT_EQ(motion[0].boot_epoch, epoch);  // original identity: hub dedups
  EXPECT_TRUE(motion[0].flag);
}

TEST(Power, LowBatteryEntersLowPowerAndStretchesHeartbeat) {
  Node n(default_board());
  n.power.source_ = PowerSource::kBatteryOnly;
  n.boot();
  n.run_for(5);
  n.drain_and_ack();
  n.power.battery_mv_ = 3500;
  n.run_for(60'000);
  auto events = n.drain_and_ack();
  ASSERT_EQ(of_kind(events, EventKind::kPowerMode).size(), 1u);
  EXPECT_EQ(n.runtime().power_mode(), PowerMode::kLowPower);
  n.run_for(180'000);  // < 4 x 60 s: no heartbeat yet
  EXPECT_TRUE(of_kind(n.drain_and_ack(), EventKind::kHeartbeat).empty());
  n.run_for(61'000);
  EXPECT_EQ(of_kind(n.drain_and_ack(), EventKind::kHeartbeat).size(), 1u);
}

// --- OTA
// ------------------------------------------------------------------------------------------

std::vector<std::uint8_t> image(std::size_t n, std::uint8_t seed) {
  std::vector<std::uint8_t> v(n);
  for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(i * 31 + seed);
  return v;
}

OtaOffer offer_for(const std::vector<std::uint8_t>& img, std::uint32_t version) {
  OtaOffer o;
  o.version = version;
  o.size = static_cast<std::uint32_t>(img.size());
  o.sha256 = talos::sha256(std::string_view(reinterpret_cast<const char*>(img.data()), img.size()));
  return o;
}

void send_chunks(Node& n, const std::vector<std::uint8_t>& img, std::uint32_t from,
                 std::uint32_t to) {
  for (std::uint32_t off = from; off < to; off += kOtaChunkMax) {
    OtaChunk c;
    c.offset = off;
    c.length = static_cast<std::uint16_t>(std::min<std::size_t>(kOtaChunkMax, img.size() - off));
    std::copy_n(img.begin() + off, c.length, c.data.begin());
    n.link.downlink.push_back(c);
    n.run_for(1);
  }
}

TEST(Ota, HappyPathTrialBootThenConfirmOnHubContact) {
  Node n(default_board());
  n.boot();
  auto img = image(4000, 7);
  n.link.downlink.push_back(offer_for(img, 2));
  n.run_for(2);
  EXPECT_EQ(n.runtime().ota().state, OtaState::kDownloading);
  send_chunks(n, img, 0, 4000);
  n.run_for(5);
  EXPECT_EQ(n.reboots(), 1u);  // reboot into the new slot
  EXPECT_EQ(n.runtime().ota().state, OtaState::kPendingVerify);
  EXPECT_EQ(n.runtime().running_version(), 2u);
  n.drain_and_ack();  // the hub hears from the new image
  n.run_for(5);
  EXPECT_EQ(n.runtime().ota().state, OtaState::kIdle);
  EXPECT_EQ(n.runtime().ota().active_slot, 1);
  EXPECT_EQ(n.runtime().ota().active_version, 2u);
}

TEST(Ota, RollbackWhenTrialImageRebootsBeforeConfirming) {
  Node n(default_board());
  n.boot();
  auto img = image(1000, 3);
  n.link.downlink.push_back(offer_for(img, 2));
  n.run_for(2);
  send_chunks(n, img, 0, 1000);
  n.run_for(5);
  ASSERT_EQ(n.runtime().ota().state, OtaState::kPendingVerify);
  n.link.uplink.clear();  // the new image never reaches the hub
  n.boot();               // watchdog / crash reboot
  EXPECT_EQ(n.runtime().ota().state, OtaState::kRolledBack);
  EXPECT_EQ(n.runtime().running_version(), 1u);
  EXPECT_EQ(n.runtime().ota().active_slot, 0);
  n.run_for(5);
  auto status = of_kind(n.drain_and_ack(), EventKind::kOtaStatus);
  ASSERT_FALSE(status.empty());
  EXPECT_EQ(status.front().a, static_cast<std::int32_t>(OtaState::kRolledBack));
}

TEST(Ota, RejectsBadDigestDowngradeOversizeAndLowBattery) {
  Node n(default_board());
  n.boot();
  auto img = image(500, 1);
  auto bad = offer_for(img, 2);
  bad.sha256[0] ^= 0xFF;
  n.link.downlink.push_back(bad);
  n.run_for(2);
  send_chunks(n, img, 0, 500);
  EXPECT_EQ(n.runtime().ota().state, OtaState::kFailed);
  EXPECT_EQ(n.reboots(), 0u);

  Node d(default_board());
  d.boot();
  d.link.downlink.push_back(offer_for(img, 1));  // not newer than running v1
  d.run_for(2);
  EXPECT_EQ(d.runtime().ota().state, OtaState::kIdle);
  OtaOffer huge = offer_for(img, 3);
  huge.size = d.flash.slot_capacity() + 1;
  d.link.downlink.push_back(huge);
  d.run_for(2);
  EXPECT_EQ(d.runtime().ota().state, OtaState::kIdle);

  Node low(default_board());
  low.power.source_ = PowerSource::kBatteryOnly;
  low.power.battery_mv_ = 3600;
  low.boot();
  low.link.downlink.push_back(offer_for(img, 2));
  low.run_for(2);
  EXPECT_EQ(low.runtime().ota().state, OtaState::kIdle);
}

TEST(Ota, ResumesAfterResetMidDownloadAndRejectsOutOfOrderChunks) {
  Node n(default_board());
  n.boot();
  auto img = image(2048, 9);
  n.link.downlink.push_back(offer_for(img, 2));
  n.run_for(2);
  send_chunks(n, img, 0, 1024);
  n.boot();  // reset mid-download
  EXPECT_EQ(n.runtime().ota().state, OtaState::kDownloading);
  EXPECT_EQ(n.runtime().ota().received, 1024u);
  send_chunks(n, img, 1280, 1408);  // gap: ignored
  EXPECT_EQ(n.runtime().ota().received, 1024u);
  send_chunks(n, img, 1024, 2048);
  n.run_for(5);
  EXPECT_EQ(n.runtime().ota().state, OtaState::kPendingVerify);
}

// Reset injection at every persisted OTA transition: a power cut that loses
// the write leaves the device on a consistent, recoverable state.
TEST(Ota, PowerCutAtAnyStoreWriteNeverBricks) {
  for (int cut_at = 1; cut_at <= 40; ++cut_at) {
    Node n(default_board());
    n.boot();
    auto img = image(1024, static_cast<std::uint8_t>(cut_at));
    const auto base = n.store.puts;
    n.link.downlink.push_back(offer_for(img, 2));
    for (std::uint32_t off = 0, step = 0; off <= 1024; ++step) {
      if (n.store.puts - base + 1 == static_cast<std::uint32_t>(cut_at)) n.store.fail_puts = 1;
      if (off < 1024) {
        OtaChunk c;
        c.offset = off;
        c.length = kOtaChunkMax;
        std::copy_n(img.begin() + off, c.length, c.data.begin());
        n.link.downlink.push_back(c);
      }
      n.run_for(1);
      off += kOtaChunkMax;
      if (step > 20) break;
    }
    n.boot();  // power returns
    const auto st = n.runtime().ota().state;
    EXPECT_TRUE(st == OtaState::kIdle || st == OtaState::kDownloading ||
                st == OtaState::kPendingVerify || st == OtaState::kRolledBack ||
                st == OtaState::kReadyToBoot)
        << "cut " << cut_at;
    // Whatever happened, the running version is a real, complete image.
    EXPECT_TRUE(n.runtime().running_version() == 1u || n.runtime().running_version() == 2u);
  }
}

}  // namespace
}  // namespace talos::fw::sim
