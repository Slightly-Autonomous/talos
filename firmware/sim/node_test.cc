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

// Runs with the hub answering every second. A hub that stays silent for
// minutes is a link outage, and the node then rations non-critical sends.
std::vector<DeviceEvent> run_acked(Node& n, TimeMs ms) {
  std::vector<DeviceEvent> out;
  for (TimeMs t = 0; t < ms; t += 1'000) {
    n.run_for(std::min<TimeMs>(1'000, ms - t));
    for (const auto& e : n.drain_and_ack()) out.push_back(e);
  }
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
  auto closed = field(axis == 1 ? -30'000 : 30'000);  // either magnet polarity
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
  auto wobble = field(axis == 1 ? -15'000 : 15'000);  // between the thresholds: still closed
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
  EXPECT_EQ(Tmag5273ContactDriver::threshold_code(Tmag5273ContactDriver::kOpenUt), 32);   // 10 mT
  EXPECT_EQ(Tmag5273ContactDriver::threshold_code(Tmag5273ContactDriver::kCloseUt), 64);  // 20 mT
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
  auto events = run_acked(n, PirDriver::kWarmMaxMs + 1'000);
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
  auto faults = of_kind(run_acked(n, 300'000), EventKind::kModuleFault);
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
    auto f = of_kind(run_acked(m, 300'100), EventKind::kModuleFault);
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
  n.hall(0).set_field(24'000, 20'000, 0);  // magnet at an angle: X dominant, Y strong too
  n.boot();
  n.run_for(5);
  n.drain_and_ack();
  // Door opens. Only X is armed while closed, so Y's result register still
  // holds 20 mT from the last full conversion.
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
  n.id(0).blank();                                                        // never programmed
  n.id(1).regs[15] = static_cast<std::uint8_t>(n.id(1).regs[15] ^ 0x5A);  // CRC broken
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

// --- TL-SN-001 board I/O (expanders, rails, presence, source, charger) ------------------------

Node rev_a() { return Node(tl_sn_001_board()); }

// SlotPowerBlob in runtime.cc: powering, quarantined mask, strikes per slot.
using SlotPowerRecord = std::array<std::uint8_t, 2 + kSlots>;

SlotPowerRecord slot_power_record(Node& n) {
  SlotPowerRecord rec{};
  EXPECT_EQ(n.store.get(StoreKey::kSlotPower, rec.data(), rec.size()), rec.size());
  return rec;
}

TEST(BoardIo, BootLeavesEveryOutputSafeAndNeverPulsesARail) {
  Node n = rev_a();
  n.boot();
  EXPECT_TRUE(n.runtime().board_ok());
  for (std::uint8_t s = 0; s < 4; ++s) {  // no cards: every slot stays unpowered
    EXPECT_FALSE(n.rev_a().aon_on(s)) << int(s);
    EXPECT_FALSE(n.rev_a().sw_on(s)) << int(s);
    EXPECT_FALSE(n.rev_a().reset_n(s)) << int(s);
    EXPECT_FALSE(n.rev_a().cs_n(s)) << int(s);
  }
  // Output latches were written before the pins became outputs: no rail enable ever drove high.
  EXPECT_EQ(n.rev_a().exp_a.ever_driven_high & 0x00FF, 0);
  EXPECT_TRUE(n.rev_a().exp_b.is_output(10));  // UART mux enable, parked high
  EXPECT_TRUE(n.rev_a().exp_b.level(10));
  EXPECT_FALSE(n.rev_a().exp_b.level(11));  // presence probe idles low (no pull-up current)
}

std::vector<DeviceEvent> faults_of(Node& n, ModuleFault f) {
  std::vector<DeviceEvent> out;
  n.run_for(20);  // let the runtime transmit what it queued
  for (const auto& e : n.drain_and_ack()) {
    if (e.kind == EventKind::kModuleFault && e.a == static_cast<std::int32_t>(f)) out.push_back(e);
  }
  return out;
}

TEST(SlotPower, SeatedSlotsArePoweredEmptyOnesStayOff) {
  Node n = rev_a();
  n.insert(0, ModuleCode::kSht40);
  n.insert(2, ModuleCode::kLis2dw12);
  n.boot();
  EXPECT_TRUE(n.rev_a().aon_on(0));
  EXPECT_FALSE(n.rev_a().aon_on(1));
  EXPECT_TRUE(n.rev_a().aon_on(2));
  EXPECT_FALSE(n.rev_a().aon_on(3));
  EXPECT_TRUE(n.runtime().slot(0).enabled);
  EXPECT_TRUE(n.runtime().slot(2).enabled);
  EXPECT_TRUE(n.rev_a().cs_n(0));  // lines deasserted once AON is up
  EXPECT_EQ(n.runtime().quarantined_slots(), 0);
  // The attempt marker is cleared after each slot came up.
  EXPECT_EQ(slot_power_record(n)[0], NodeRuntime::kNotPowering);
}

TEST(SlotPower, OneResetWhilePoweringIsAStrikeNotAQuarantine) {
  Node n = rev_a();
  n.insert(1, ModuleCode::kSht40);
  // A USB unplug or watchdog reset landed inside slot 1's power-on window once.
  const SlotPowerRecord rec = {1, 0, 0, 0, 0, 0};
  ASSERT_TRUE(n.store.put(StoreKey::kSlotPower, rec.data(), rec.size()));
  n.boot();
  EXPECT_EQ(n.runtime().quarantined_slots(), 0);
  EXPECT_TRUE(n.runtime().slot(1).enabled);  // retried at once, and it came up
  EXPECT_EQ(n.runtime().power_strikes(1), 0);
  EXPECT_EQ(slot_power_record(n), (SlotPowerRecord{NodeRuntime::kNotPowering, 0, 0, 0, 0, 0}));
}

TEST(SlotPower, QuarantineIsRetriedWithBackoffAndHealsAFalsePositive) {
  Node n = rev_a();
  n.insert(1, ModuleCode::kSht40);
  // Third reset in a row while powering slot 1: quarantined, retry after 2 h, not 1 h.
  const SlotPowerRecord rec = {1, 0b0010, 0, 2, 0, 0};
  ASSERT_TRUE(n.store.put(StoreKey::kSlotPower, rec.data(), rec.size()));
  n.boot();
  EXPECT_EQ(n.runtime().quarantined_slots(), 0b0010);
  EXPECT_EQ(n.runtime().power_strikes(1), 3);
  n.run_for(NodeRuntime::kQuarantineRetryMs + 1000);
  EXPECT_FALSE(n.rev_a().aon_on(1));
  EXPECT_EQ(n.runtime().quarantined_slots(), 0b0010);
  n.run_for(NodeRuntime::kQuarantineRetryMs);
  // Retried without anyone at the USB console; the card powered up fine.
  EXPECT_EQ(n.runtime().quarantined_slots(), 0);
  EXPECT_TRUE(n.rev_a().aon_on(1));
  EXPECT_TRUE(n.runtime().slot(1).enabled);
  EXPECT_EQ(slot_power_record(n), (SlotPowerRecord{NodeRuntime::kNotPowering, 0, 0, 0, 0, 0}));
  n.boot();
  EXPECT_TRUE(n.runtime().slot(1).enabled);
}

TEST(SlotPower, CardsThatCannotBeUsedAreNotLeftOnAon) {
  Node n = rev_a();
  n.insert(0, ModuleCode::kTlMod004Env);
  n.id(0).blank();
  n.insert(1, static_cast<ModuleCode>(0x7A7A));  // not in the registry
  n.insert(2, ModuleCode::kTlMod004Env);
  n.i2c.detach(2, Sht40Driver::kAddr);  // ID fine, sensor dead: init fails
  n.insert(3, ModuleCode::kTlMod004Env);
  n.i2c.detach(3, 0x50);  // seated, ID dead: stays up so the bench can scan the card
  n.boot();
  EXPECT_EQ(n.runtime().slot(0).fault, ModuleFault::kIdBlank);
  EXPECT_EQ(n.runtime().slot(1).fault, ModuleFault::kUnknownModule);
  EXPECT_EQ(n.runtime().slot(2).fault, ModuleFault::kInitFailed);
  EXPECT_EQ(n.runtime().slot(3).fault, ModuleFault::kNoIdEeprom);
  for (std::uint8_t s = 0; s < 3; ++s) {
    EXPECT_FALSE(n.rev_a().aon_on(s)) << int(s);
    EXPECT_FALSE(n.rev_a().cs_n(s)) << int(s);
    EXPECT_FALSE(n.board_io().rail_on(s, Rail::kAlwaysOn3v3)) << int(s);
  }
  EXPECT_TRUE(n.rev_a().aon_on(3));
  EXPECT_EQ(n.i2c.mask(), 0);                      // deselected before the rails went off
  n.i2c.attach(2, Sht40Driver::kAddr, &n.sht(2));  // reseated: a rescan powers it again
  n.runtime().rescan(2);
  EXPECT_TRUE(n.rev_a().aon_on(2));
  EXPECT_TRUE(n.runtime().slot(2).enabled);
}

TEST(SlotPower, ResetWhilePoweringASlotQuarantinesItInsteadOfBootLooping) {
  Node n = rev_a();
  n.insert(1, ModuleCode::kSht40);
  n.insert(3, ModuleCode::kSht40);
  // What a second reset in a row in the middle of switching slot 1's AON on leaves behind.
  const SlotPowerRecord rec = {1, 0, 0, 1, 0, 0};
  ASSERT_TRUE(n.store.put(StoreKey::kSlotPower, rec.data(), rec.size()));
  n.boot();
  EXPECT_FALSE(n.rev_a().aon_on(1));
  EXPECT_EQ(n.runtime().slot(1).fault, ModuleFault::kPowerQuarantined);
  EXPECT_FALSE(n.runtime().slot(1).enabled);
  EXPECT_TRUE(n.runtime().slot(3).enabled);  // the other slots come up as usual
  EXPECT_EQ(faults_of(n, ModuleFault::kPowerQuarantined).size(), 1u);
  n.boot();  // the quarantine survives a reboot
  EXPECT_FALSE(n.rev_a().aon_on(1));
  EXPECT_EQ(n.runtime().quarantined_slots(), 0b0010);
  n.runtime().clear_quarantine(1);  // operator action
  n.runtime().rescan(1);
  EXPECT_TRUE(n.rev_a().aon_on(1));
  EXPECT_TRUE(n.runtime().slot(1).enabled);
  n.boot();
  EXPECT_TRUE(n.runtime().slot(1).enabled);
}

TEST(SlotPower, SwitchedRailFaultTurnsRailsOffAndIsReported) {
  Node n = rev_a();
  n.insert(0, ModuleCode::kTlTest001);
  n.insert(1, ModuleCode::kTlTest001);
  n.boot();
  n.drain_and_ack();
  n.board_io().set_rail(0, Rail::kSwitched3v3, true);
  n.board_io().set_rail(1, Rail::kSwitched3v3, true);
  n.run_for(50);
  EXPECT_TRUE(faults_of(n, ModuleFault::kRailFault).empty());  // no fault, nothing happens
  n.rev_a().sw_overload[1] = true;
  n.runtime().on_interrupt();
  n.run_for(50);
  // FAULT_N is wired-OR: both rails go off, AON stays up, and both slots are reported.
  EXPECT_FALSE(n.rev_a().sw_on(0));
  EXPECT_FALSE(n.rev_a().sw_on(1));
  EXPECT_TRUE(n.rev_a().aon_on(1));
  const auto f = faults_of(n, ModuleFault::kRailFault);
  ASSERT_EQ(f.size(), 2u);
  EXPECT_EQ(f[0].slot, 0);
  EXPECT_EQ(f[1].slot, 1);
}

TEST(MuxReset, CardHoldingTheBusIsCutOffByTheMuxReset) {
  for (const bool wired : {true, false}) {
    BoardSupport b = tl_sn_001_board();
    if (!wired) b.mux_reset_line = 0xFF;
    Node n(b);
    n.insert(0, ModuleCode::kSht40);
    n.boot();
    ASSERT_EQ(n.board_io().source(), PowerSource::kUsb);
    n.i2c.stick_branch(0, true);  // the card starts holding SDA low
    n.runtime().rescan(0);
    if (wired) {
      EXPECT_GT(n.i2c.mux_resets, 0u);
      EXPECT_EQ(n.i2c.mask(), 0);
      EXPECT_EQ(n.board_io().source(), PowerSource::kUsb);  // root bus free again
    } else {
      EXPECT_EQ(n.board_io().source(), PowerSource::kUnknown);  // only a power cycle helps
    }
  }
}

TEST(SlotPower, AdmissionBudgetsFitTheRegulator) {
  // Every module rail on TL-SN-001 comes from the TPS63900: 0.4 A x 3.3 V = 1320 mW guaranteed,
  // ~500 mW of it for the MCU's TX burst. No power source may admit more than the rest.
  const BoardSupport b = tl_sn_001_board();
  constexpr std::uint32_t kRegulatorMw = 400 * 3300 / 1000, kMcuTxMw = 123 * 3300 / 1000 + 100;
  for (std::uint32_t budget : {b.usb_sustained_mw, b.solar_sustained_mw, b.battery_sustained_mw}) {
    EXPECT_LE(budget, kRegulatorMw - kMcuTxMw);
  }
}

TEST(BoardIo, WarmResetWithAFailedBeginNeverSwitchesEveryRailOn) {
  Node n = rev_a();
  n.insert(0, ModuleCode::kTlMod004Env);
  n.boot();
  ASSERT_TRUE(n.rev_a().aon_on(0));
  // MCU warm reset: the expanders keep their pins as outputs, and the root bus
  // glitches while begin() runs.
  n.rev_a().exp_a.nack = true;
  n.boot();
  EXPECT_FALSE(n.runtime().board_ok());
  EXPECT_FALSE(n.board_io().expanders_ready());
  n.rev_a().exp_a.nack = false;
  n.rev_a().exp_a.ever_driven_high = 0;
  n.board_io().set_rail(1, Rail::kAlwaysOn3v3, true);
  // Only slot 1's AON came on: no other enable, and no card line went high early.
  EXPECT_TRUE(n.board_io().expanders_ready());  // configured on the retry
  EXPECT_TRUE(n.rev_a().aon_on(1));
  for (const std::uint8_t s : {std::uint8_t{0}, std::uint8_t{2}, std::uint8_t{3}}) {
    EXPECT_FALSE(n.rev_a().aon_on(s)) << int(s);
    EXPECT_FALSE(n.rev_a().reset_n(s)) << int(s);
  }
  for (std::uint8_t s = 0; s < 4; ++s) EXPECT_FALSE(n.rev_a().sw_on(s)) << int(s);
  EXPECT_EQ(n.rev_a().exp_a.ever_driven_high & ~0x2220, 0);  // AON_EN1, RESET_N1, CS_N1 only
}

TEST(BoardIo, RailsAndUartAreRefusedUntilTheExpandersTookTheirSafeState) {
  Node n = rev_a();
  n.insert(0, ModuleCode::kTlTest001);
  n.rev_a().exp_b.nack = true;  // EXP_B (UART mux, presence) never configured
  n.boot();
  EXPECT_FALSE(n.board_io().expanders_ready());
  n.board_io().set_rail(0, Rail::kAlwaysOn3v3, true);
  EXPECT_FALSE(n.rev_a().aon_on(0));
  EXPECT_FALSE(n.board_io().rail_on(0, Rail::kAlwaysOn3v3));
  EXPECT_FALSE(n.board_io().select_uart(0));
  n.rev_a().exp_b.nack = false;  // bus recovered: the next request configures and proceeds
  n.board_io().set_rail(0, Rail::kAlwaysOn3v3, true);
  EXPECT_TRUE(n.board_io().expanders_ready());
  EXPECT_TRUE(n.rev_a().aon_on(0));
  EXPECT_TRUE(n.rev_a().exp_b.level(10));  // UART mux parked by the configuration
}

TEST(BoardIo, CardLinesWaitForTheAonRail) {
  Node n = rev_a();
  // TPS22917 tON with C_CT = 1 nF is ~3.8 ms typ; model a slow part at 2x.
  n.rev_a().aon_rise_ms = 8;
  for (std::uint8_t s = 0; s < 4; ++s) n.insert(s, ModuleCode::kTlMod004Env);
  n.boot();
  for (std::uint8_t s = 0; s < 4; ++s) {
    EXPECT_TRUE(n.runtime().slot(s).enabled) << int(s);
    EXPECT_TRUE(n.rev_a().cs_n(s)) << int(s);
    EXPECT_FALSE(n.rev_a().backfed[s]) << "RESET_N/CS_N high before AON was up, slot " << int(s);
  }
  n.board_io().set_rail(2, Rail::kAlwaysOn3v3, false);
  n.board_io().set_rail(2, Rail::kAlwaysOn3v3, true);
  EXPECT_FALSE(n.rev_a().backfed[2]);
  EXPECT_GE(BoardIo::kAonSettleMs, 8u);
}

TEST(BoardIo, SharedSpiLinesAreDeclaredForParking) {
  // esp::Gpio parks these at boot (SCK/MOSI low, MISO pulled down). They must
  // match the pin ledger (IO4/IO5/IO0) and never collide with another use.
  const BoardSupport b = tl_sn_001_board();
  EXPECT_EQ(b.spi_sck_line, 4);
  EXPECT_EQ(b.spi_mosi_line, 5);
  EXPECT_EQ(b.spi_miso_line, 0);
  for (const auto line : {b.spi_sck_line, b.spi_mosi_line, b.spi_miso_line}) {
    EXPECT_NE(line, b.mux_reset_line);
    for (const auto& s : b.slots) EXPECT_NE(line, s.wake_line);
  }
}

TEST(BoardIo, SeatedCardWithDeadIdIsAFaultEmptySlotIsNot) {
  Node n = rev_a();
  n.insert(0, ModuleCode::kTlMod004Env);
  n.insert(1, ModuleCode::kTlMod004Env);
  n.i2c.detach(1, 0x50);  // card seated, its ID EEPROM not answering
  n.boot();
  EXPECT_TRUE(n.runtime().slot(0).enabled);
  EXPECT_EQ(n.runtime().slot(1).fault, ModuleFault::kNoIdEeprom);
  EXPECT_EQ(n.runtime().slot(2).fault, ModuleFault::kNone);  // empty
  EXPECT_FALSE(n.rev_a().exp_b.level(11));
}

TEST(BoardIo, RailSequencingFollowsS2AndS3) {
  Node n = rev_a();
  n.insert(0, ModuleCode::kTlTest001);
  n.boot();
  BoardIo& io = n.board_io();
  io.set_rail(0, Rail::kSwitched3v3, true);
  EXPECT_TRUE(n.rev_a().sw_on(0));
  io.set_rail(0, Rail::kAlwaysOn3v3, false);  // SW goes off first, card lines go low
  EXPECT_FALSE(n.rev_a().sw_on(0));
  EXPECT_FALSE(n.rev_a().aon_on(0));
  EXPECT_FALSE(n.rev_a().cs_n(0));
  EXPECT_FALSE(n.rev_a().reset_n(0));
  io.set_rail(0, Rail::kSwitched3v3, true);  // refused: AON is off
  EXPECT_FALSE(n.rev_a().sw_on(0));
  EXPECT_FALSE(io.select_uart(0));  // never route the UART into an unpowered slot
  io.set_rail(0, Rail::kAlwaysOn3v3, true);
  EXPECT_TRUE(n.rev_a().aon_on(0));
  EXPECT_TRUE(n.rev_a().cs_n(0));
  EXPECT_TRUE(io.select_uart(0));
  EXPECT_FALSE(n.rev_a().exp_b.level(10));  // mux enabled
  io.set_rail(0, Rail::kAlwaysOn3v3, false);
  EXPECT_FALSE(io.uart_slot().has_value());  // rail off parks the mux first
  EXPECT_TRUE(n.rev_a().exp_b.level(10));
}

TEST(BoardIo, UnpoweredSlotsDevicesDoNotAnswer) {
  Node n = rev_a();
  n.insert(2, ModuleCode::kTlMod004Env);
  n.boot();
  auto read_id = [&] {
    const std::uint8_t mask = 1u << 2, reg = 0;
    std::uint8_t b = 0;
    const bool ok = n.i2c.write(0x70, &mask, 1) && n.i2c.write_read(0x50, &reg, 1, &b, 1);
    const std::uint8_t none = 0;
    n.i2c.write(0x70, &none, 1);
    return ok;
  };
  EXPECT_TRUE(read_id());
  n.board_io().set_rail(2, Rail::kAlwaysOn3v3, false);
  EXPECT_FALSE(read_id());
  n.board_io().set_rail(2, Rail::kAlwaysOn3v3, true);
  EXPECT_TRUE(read_id());
}

TEST(BoardIo, PowerSourceFromChargerAndMuxStatusUnknownWhenUnreadable) {
  Node n = rev_a();
  n.boot();
  EXPECT_EQ(n.board_io().source(), PowerSource::kUsb);
  n.rev_a().source = RevABoard::Source::kSolar;
  EXPECT_EQ(n.board_io().source(), PowerSource::kSolarBattery);
  n.rev_a().charger.vin_good = false;
  n.rev_a().source = RevABoard::Source::kBatteryOnly;
  EXPECT_EQ(n.board_io().source(), PowerSource::kBatteryOnly);
  n.rev_a().exp_b.nack = true;
  EXPECT_EQ(n.board_io().source(), PowerSource::kUnknown);  // never assumed to be USB
}

TEST(BoardIo, ChargerConfiguredExplicitlyAndReappliedAfterARegisterReset) {
  Node n = rev_a();
  n.boot();
  auto& regs = n.rev_a().charger.regs;
  EXPECT_EQ(regs[Bq25186::kIchgCtrl], Bq25186::ichg_code(300));  // USB: 300 mA
  EXPECT_EQ(Bq25186::ichg_code(300), 0x39);
  EXPECT_EQ(Bq25186::ichg_code(200), 0x2F);
  EXPECT_EQ(Bq25186::ichg_code(10), 0x05);
  EXPECT_EQ(regs[Bq25186::kShipRst], 0x00);        // no ship mode from a shorted TS
  EXPECT_EQ(regs[Bq25186::kSysReg] & 0xE0, 0x40);  // SYS 4.5 V, never pass-through
  EXPECT_EQ(regs[Bq25186::kIcCtrl] & 0x03, 0x03);  // watchdog off: config does not revert
  n.rev_a().charger.reset_registers();             // brown-out / watchdog
  n.run_for(61'000);                               // next heartbeat services the board
  EXPECT_EQ(regs[Bq25186::kIchgCtrl], Bq25186::ichg_code(300));
  EXPECT_EQ(regs[Bq25186::kShipRst], 0x00);
  // On solar the charge current drops to 200 mA at the next service.
  n.rev_a().source = RevABoard::Source::kSolar;
  n.run_for(61'000);
  EXPECT_EQ(regs[Bq25186::kIchgCtrl], Bq25186::ichg_code(200));
  EXPECT_TRUE(n.board_io().charger_ok());
}

TEST(BoardIo, DeadChargerIsReportedNotHidden) {
  Node n = rev_a();
  n.rev_a().charger.nack = true;
  n.boot();
  EXPECT_FALSE(n.runtime().board_ok());
  EXPECT_FALSE(n.board_io().charger_ok());
  EXPECT_FALSE(n.board_io().charger_status().has_value());
}

TEST(BoardIo, UnknownSourceBudgetsLikeBattery) {
  Node n = rev_a();
  n.adc.mv = 3400;  // low battery
  n.rev_a().exp_b.nack = true;
  n.boot();
  EXPECT_EQ(n.runtime().power_mode(), PowerMode::kLowPower);
}

TEST(BoardIo, TestCardIsDiscoveredWithEveryPinReleased) {
  Node n = rev_a();
  n.insert(3, ModuleCode::kTlTest001);
  n.boot();
  EXPECT_TRUE(n.runtime().slot(3).enabled);
  EXPECT_EQ(n.runtime().slot(3).code, ModuleCode::kTlTest001);
  for (std::uint8_t pin = 0; pin < 16; ++pin) EXPECT_FALSE(n.test_card(3).is_output(pin));
}

TEST(IdBlock, GoldenVectors) {
  // Same vectors as tools/dbg/tests/test_bench.py, so the two encoders never drift.
  const std::uint8_t beef[2] = {0xBE, 0xEF};
  EXPECT_EQ(crc8(beef, 2), 0x92);  // Sensirion's published check value
  const auto block = make_id_block(ModuleCode::kTlMod004Env, 1);
  const std::array<std::uint8_t, 16> want = {0x03, 0x02, 0x01, 0x01, 0x00, 0xFF, 0xFF, 0xFF,
                                             0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xB0};
  EXPECT_EQ(block, want);
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

TEST(Delivery, LowerPriorityNeverEvictsHigher) {
  auto ev = [](std::uint32_t seq, Priority p) {
    DeviceEvent e;
    e.seq = seq;
    e.priority = p;
    return e;
  };
  auto kept = [](const auto& q) {
    std::vector<std::uint32_t> v;
    q.for_each([&](const DeviceEvent& e) { v.push_back(e.seq); });
    std::sort(v.begin(), v.end());
    return v;
  };
  EventQueue<4> q;
  for (std::uint32_t i = 1; i <= 4; ++i) q.push(ev(i, Priority::kP0Critical));
  EXPECT_FALSE(q.push(ev(5, Priority::kP2Health)));  // a heartbeat never evicts an alarm
  EXPECT_EQ(q.dropped(), 1u);
  EXPECT_EQ(q.dropped_p0(), 0u);
  EXPECT_EQ(kept(q), (std::vector<std::uint32_t>{1, 2, 3, 4}));

  EventQueue<2> r;
  r.push(ev(1, Priority::kP1State));
  r.push(ev(2, Priority::kP0Critical));
  EXPECT_FALSE(r.push(ev(3, Priority::kP2Health)));  // nor a contact state change
  EXPECT_EQ(kept(r), (std::vector<std::uint32_t>{1, 2}));
  EXPECT_TRUE(r.push(ev(4, Priority::kP1State)));  // same priority: the oldest makes room
  EXPECT_EQ(kept(r), (std::vector<std::uint32_t>{2, 4}));
  EXPECT_EQ(r.dropped(), 2u);
}

TEST(Persistence, EpochThatCannotBePersistedHoldsTheUplinkAndNeverRepeats) {
  Node n(default_board());
  n.boot();
  const auto e1 = n.runtime().boot_epoch();
  n.run_for(5);
  n.drain_and_ack();
  n.store.fail_puts = 1000;  // NVS full or unwritable across the next reset
  n.boot();
  EXPECT_FALSE(n.runtime().boot_epoch_persisted());
  n.run_for(3000);
  EXPECT_TRUE(n.link.uplink.empty());  // (e1 + 1, seq) is not on record: hold it
  n.store.fail_puts = 0;
  n.run_for(NodeRuntime::kEpochRetryMs + 10);
  EXPECT_TRUE(n.runtime().boot_epoch_persisted());
  const auto held = n.drain_and_ack();
  ASSERT_FALSE(held.empty());
  EXPECT_EQ(held.front().boot_epoch, e1 + 1);
  EXPECT_EQ(held.front().seq, 1u);
  n.boot();
  EXPECT_EQ(n.runtime().boot_epoch(), e1 + 2);  // strictly increasing
}

TEST(Persistence, ExhaustedEpochFailsLoudlyInsteadOfWrapping) {
  Node n(default_board());
  const std::uint32_t last = 0xFFFFFFFEu;
  ASSERT_TRUE(
      n.store.put(StoreKey::kBootEpoch, reinterpret_cast<const std::uint8_t*>(&last), sizeof last));
  n.boot();
  EXPECT_FALSE(n.runtime().boot_epoch_persisted());
  n.run_for(3000);
  EXPECT_TRUE(n.link.uplink.empty());
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
  auto img = image(3 * Ota::kSaveEveryBytes, 9);
  n.link.downlink.push_back(offer_for(img, 2));
  n.run_for(2);
  const std::uint32_t cut = Ota::kSaveEveryBytes + 1024;
  send_chunks(n, img, 0, cut);
  n.boot();  // reset mid-download: resumes from the last saved offset
  EXPECT_EQ(n.runtime().ota().state, OtaState::kDownloading);
  EXPECT_EQ(n.runtime().ota().received, Ota::kSaveEveryBytes);
  send_chunks(n, img, cut, cut + 128);  // gap: ignored
  EXPECT_EQ(n.runtime().ota().received, Ota::kSaveEveryBytes);
  send_chunks(n, img, Ota::kSaveEveryBytes, static_cast<std::uint32_t>(img.size()));
  n.run_for(5);
  EXPECT_EQ(n.runtime().ota().state, OtaState::kPendingVerify);
  EXPECT_EQ(n.runtime().running_version(), 2u);  // the digest held over the rewritten range
}

TEST(Ota, DownloadProgressIsPersistedSparingly) {
  Node n(default_board());
  n.boot();
  n.run_for(5);
  n.drain_and_ack();
  auto img = image(4 * Ota::kSaveEveryBytes, 5);
  n.link.downlink.push_back(offer_for(img, 2));
  n.run_for(2);
  const auto before = n.store.puts;
  send_chunks(n, img, 0, 4 * Ota::kSaveEveryBytes - kOtaChunkMax);
  // 127 chunks: one OTA record per 4 KiB instead of one per chunk.
  EXPECT_LE(n.store.puts - before, 4u);
}

TEST(Ota, TrialImageIsMarkedValidOnlyAfterTheHealthCheck) {
  auto trial = [](Node& n) {
    n.boot();
    auto img = image(1000, 4);
    n.link.downlink.push_back(offer_for(img, 2));
    n.run_for(2);
    send_chunks(n, img, 0, 1000);
    n.run_for(5);
    ASSERT_EQ(n.runtime().ota().state, OtaState::kPendingVerify);
  };
  Node ok(tl_sn_001_board());
  trial(ok);
  ok.run_for(20);
  EXPECT_EQ(ok.flash.valid_marks, 0u);  // booted is not healthy yet
  ok.drain_and_ack();
  ok.run_for(5);
  EXPECT_EQ(ok.runtime().ota().state, OtaState::kIdle);
  EXPECT_EQ(ok.flash.valid_marks, 1u);

  Node no_bootloader(tl_sn_001_board());
  trial(no_bootloader);
  no_bootloader.flash.fail_mark_valid = true;  // bootloader can't be told: stay pending
  no_bootloader.drain_and_ack();
  no_bootloader.run_for(5);
  EXPECT_EQ(no_bootloader.runtime().ota().state, OtaState::kPendingVerify);
}

TEST(Ota, BoardNotInItsSafeStateNeverConfirmsATrialImage) {
  Node n(tl_sn_001_board());
  n.boot();
  n.rev_a().charger.nack = true;  // the trial image will boot with a board fault
  auto img = image(1000, 6);
  n.link.downlink.push_back(offer_for(img, 2));
  n.run_for(2);
  send_chunks(n, img, 0, 1000);
  n.run_for(5);
  ASSERT_EQ(n.runtime().ota().state, OtaState::kPendingVerify);
  ASSERT_FALSE(n.runtime().board_ok());
  n.drain_and_ack();  // reaching the hub alone is not enough
  n.run_for(5);
  EXPECT_EQ(n.runtime().ota().state, OtaState::kPendingVerify);  // the next reboot rolls back
  EXPECT_EQ(n.flash.valid_marks, 0u);
}

TEST(Watchdog, RuntimeFeedsTheTaskWatchdogEveryStep) {
  Node n(tl_sn_001_board());
  n.insert(0, ModuleCode::kTlMod004Env);
  n.esp_polling = true;  // the ESP loop: a step at least every 20 ms
  n.boot();
  const auto after_boot = n.watchdog.feeds;
  EXPECT_GE(after_boot, 4u);  // fed between slot power-ups too
  n.run_for(120'000);
  EXPECT_EQ(n.watchdog.bites, 0u);
  EXPECT_EQ(n.reboots(), 0u);
  EXPECT_GE(n.watchdog.feeds - after_boot, 120'000u / 20u);
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
