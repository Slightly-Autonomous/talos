// TL-SN-002 (ADR 0019) on the virtual board: built-in sensors as fixed slots, one
// expander (eight lamps, PIR_PWR), direct tamper / USER / USB_PRESENT lines, and
// 2 x AA primary cells. Mirrors the TL-SN-001 suites where they mean something here.
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "firmware/esp_idf/main/board_tl_sn_002.h"
#include "firmware/portable/battery.h"
#include "firmware/sim/sim_faults.h"
#include "firmware/sim/virtual_board.h"

namespace talos::fw::sim {
namespace {

namespace b2 = board::sn002;
constexpr std::uint8_t kMag = 0, kVib = 1, kPir = 2, kEnv = 3;

Node sn002() { return Node(tl_sn_002_board()); }

void on_battery(Node& n, std::uint32_t mv = 3'300) {
  n.sn002().usb = false;
  n.adc.mv = mv;
}

std::vector<DeviceEvent> of(const std::vector<DeviceEvent>& ev, EventKind k) {
  std::vector<DeviceEvent> out;
  std::copy_if(ev.begin(), ev.end(), std::back_inserter(out),
               [k](const DeviceEvent& e) { return e.kind == k; });
  return out;
}

// Runs with the hub ACKing every second, so the link never counts as failing.
std::vector<DeviceEvent> run_acked(Node& n, TimeMs ms) {
  std::vector<DeviceEvent> out;
  for (TimeMs t = 0; t < ms; t += 1'000) {
    n.run_for(std::min<TimeMs>(1'000, ms - t));
    for (const auto& e : n.drain_and_ack()) out.push_back(e);
  }
  return out;
}

// Booted and settled past the PIR warm-up, everything ACKed.
void settle(Node& n) {
  n.boot();
  n.run_for(PirDriver::kPir223d.warm_min_ms + PirDriver::kPir223d.quiet_ms + 100);
  n.drain_and_ack();
  n.run_for(10);
}

// --- boot and discovery --------------------------------------------------------------------

TEST(Sn002, BootFindsEveryBuiltInSensorWithoutIdEepromsOrAMux) {
  Node n = sn002();
  n.hall(kMag).set_field(30'000, 0, 0);  // door closed
  n.boot();
  const auto& rt = n.runtime();
  EXPECT_TRUE(rt.board_ok());
  EXPECT_EQ(rt.slot(kMag).code, ModuleCode::kTmag5273A2);
  EXPECT_EQ(rt.slot(kVib).code, ModuleCode::kLis2dw12);
  EXPECT_EQ(rt.slot(kPir).code, ModuleCode::kPir223d);
  EXPECT_EQ(rt.slot(kEnv).code, ModuleCode::kSht40);
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    EXPECT_TRUE(rt.slot(i).enabled) << int(i);
    EXPECT_EQ(rt.slot(i).fault, ModuleFault::kNone) << int(i);
    EXPECT_TRUE(rt.fixed(i));
  }
  EXPECT_TRUE(n.sn002().pir_powered());  // PIR_PWR on: warm-up started at discovery
  n.run_for(50);
  const auto ev = n.drain_and_ack();
  EXPECT_EQ(of(ev, EventKind::kModulePresent).size(), 4u);
  const auto contact = of(ev, EventKind::kContact);
  ASSERT_EQ(contact.size(), 1u);
  EXPECT_FALSE(contact[0].flag);  // closed, a boot state report
  EXPECT_EQ(contact[0].b, 1u);
  EXPECT_TRUE(of(ev, EventKind::kModuleFault).empty());
}

TEST(Sn002, ExpanderSafeStateLampsDarkUnusedLowPirRailOnlyAfterDiscovery) {
  Node n = sn002();
  n.boot();
  const Tca9535Model& e = n.sn002().exp;
  for (std::uint8_t pin = 0; pin < 8; ++pin) {
    EXPECT_TRUE(e.is_output(pin)) << int(pin);
  }
  for (std::uint8_t pin = 9; pin < 16; ++pin) {
    EXPECT_TRUE(e.is_output(pin)) << int(pin);
    EXPECT_FALSE(e.level(pin)) << int(pin);
  }
  // Unused pins never drove high: output latch written before direction.
  EXPECT_EQ(e.ever_driven_high & 0xFE00, 0);
}

TEST(Sn002, TmagA2RangeThresholdsAndReadings) {
  // 133 mT / 128 per code: 10 mT -> 10, 20 mT -> 20 (A1 would be 32 and 64).
  EXPECT_EQ(Tmag5273ContactDriver::threshold_code(Tmag5273ContactDriver::kOpenUt, 133), 10);
  EXPECT_EQ(Tmag5273ContactDriver::threshold_code(Tmag5273ContactDriver::kCloseUt, 133), 20);
  EXPECT_EQ(Tmag5273ContactDriver::threshold_code(Tmag5273ContactDriver::kOpenUt), 32);
  // A field the A1 would clip at 40 mT reads true on the A2 (magnet-defeat headroom).
  Node n = sn002();
  n.hall(kMag).set_field(90'000, 0, 0);
  n.boot();
  n.run_for(20);
  const auto contact = of(n.drain_and_ack(), EventKind::kContact);
  ASSERT_EQ(contact.size(), 1u);
  EXPECT_NEAR(contact[0].a, 90'000, 1'000);
}

TEST(Sn002, CardRemovalNeverFiresAndHeartbeatsSuperviseTheSensors) {
  Node n = sn002();
  settle(n);
  n.run_for(3 * 60'000);  // three heartbeats
  const auto faults = of(n.drain_and_ack(), EventKind::kModuleFault);
  for (const auto& f : faults) {
    EXPECT_NE(f.a, static_cast<std::int32_t>(ModuleFault::kCardRemoved));
  }
  EXPECT_TRUE(faults.empty());
}

// --- wake lines ----------------------------------------------------------------------------

TEST(Sn002Wake, FiveActiveHighLinesAreArmedInOneMask) {
  Node n = sn002();
  settle(n);
  for (const std::uint8_t line :
       {b2::kMagWake, b2::kAccInt, b2::kPirOut, b2::kTamper, b2::kUserButton}) {
    EXPECT_TRUE(n.sleep.wake_enabled(line)) << int(line);
  }
}

TEST(Sn002Wake, DoorOpenedDuringDeepSleepIsAP0Transition) {
  Node n = sn002();
  n.hall(kMag).set_field(30'000, 0, 0);
  settle(n);
  n.hall(kMag).set_field(200, 0, 0);  // asleep: the door opens, MAG_WAKE latches high
  ASSERT_TRUE(n.gpio.read(b2::kMagWake));
  n.wake_from_deep_sleep(1u << b2::kMagWake);
  EXPECT_TRUE(n.runtime().resumed_from_sleep());
  n.run_for(50);
  const auto c = of(n.drain_and_ack(), EventKind::kContact);
  ASSERT_EQ(c.size(), 1u);
  EXPECT_TRUE(c[0].flag);
  EXPECT_EQ(c[0].b, 0u);
  EXPECT_EQ(c[0].priority, Priority::kP0Critical);
}

TEST(Sn002Wake, ImpactDuringDeepSleepIsReported) {
  Node n = sn002();
  settle(n);
  n.imu(kVib).impact(1'500);
  ASSERT_TRUE(n.gpio.read(b2::kAccInt));
  n.wake_from_deep_sleep(1u << b2::kAccInt);
  n.run_for(50);
  const auto i = of(n.drain_and_ack(), EventKind::kImpact);
  ASSERT_EQ(i.size(), 1u);
  EXPECT_EQ(i[0].priority, Priority::kP0Critical);
}

TEST(Sn002Wake, MotionDuringDeepSleepKeepsThePirWarmAndIsReported) {
  Node n = sn002();
  settle(n);
  const auto cycles = n.sn002().pir_power_cycles;
  n.set_line(b2::kPirOut, true);
  ASSERT_TRUE(n.gpio.read(b2::kPirOut));
  n.wake_from_deep_sleep(1u << b2::kPirOut);
  n.run_for(100);
  const auto m = of(n.drain_and_ack(), EventKind::kMotion);
  ASSERT_GE(m.size(), 1u);
  EXPECT_TRUE(m[0].flag);
  EXPECT_EQ(n.sn002().pir_power_cycles, cycles) << "the wake must not cycle PIR_PWR";
  EXPECT_TRUE(n.sn002().pir_powered());
}

TEST(Sn002Wake, LidOpenedDuringDeepSleepWakesOnTheTamperLine) {
  Node n = sn002();
  settle(n);
  n.run_for(100);
  n.drain_and_ack();
  n.sn002().lid_open = true;
  n.wake_from_deep_sleep(1u << b2::kTamper);
  n.run_for(TamperDebouncer::kDebounceMs + 20);
  const auto t = of(n.drain_and_ack(), EventKind::kTamper);
  ASSERT_EQ(t.size(), 1u);
  EXPECT_TRUE(t[0].flag);
  EXPECT_EQ(t[0].b, 0u);
  EXPECT_EQ(t[0].priority, Priority::kP0Critical);
  // Open and reported: the line is masked so the open lid can't re-wake it forever.
  EXPECT_FALSE(n.sleep.wake_enabled(b2::kTamper));
  n.sn002().lid_open = false;
  n.run_for(TamperDebouncer::kDebounceMs + 20);
  EXPECT_TRUE(n.sleep.wake_enabled(b2::kTamper));
}

TEST(Sn002Wake, UserPressThatWokeTheNodeCountsAsAShortPress) {
  Node n = sn002();
  settle(n);
  n.sn002().user_pressed = true;
  n.wake_from_deep_sleep(1u << b2::kUserButton);
  EXPECT_FALSE(n.sleep.wake_enabled(b2::kUserButton));  // held: not a wake source
  n.run_for(300);
  n.sn002().user_pressed = false;
  n.run_for(100);
  EXPECT_TRUE(n.sleep.wake_enabled(b2::kUserButton));
  const auto u = of(n.drain_and_ack(), EventKind::kUserButton);
  ASSERT_EQ(u.size(), 1u);
  EXPECT_EQ(u[0].a, static_cast<std::int32_t>(UserButtonAction::kTestReport));
}

// --- tamper --------------------------------------------------------------------------------

TEST(Sn002Tamper, DebouncedP0OpenP1RestoreAndBounceIgnored) {
  Node n = sn002();
  settle(n);
  n.sn002().lid_open = true;
  n.run_for(20);
  n.sn002().lid_open = false;  // a 20 ms knock
  n.run_for(200);
  EXPECT_TRUE(of(n.drain_and_ack(), EventKind::kTamper).empty());
  n.sn002().lid_open = true;
  n.run_for(TamperDebouncer::kDebounceMs + 20);
  auto t = of(n.drain_and_ack(), EventKind::kTamper);
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].priority, Priority::kP0Critical);
  n.sn002().lid_open = false;
  n.run_for(TamperDebouncer::kDebounceMs + 20);
  t = of(n.drain_and_ack(), EventKind::kTamper);
  ASSERT_EQ(t.size(), 1u);
  EXPECT_FALSE(t[0].flag);
  EXPECT_EQ(t[0].priority, Priority::kP1State);
}

TEST(Sn002Tamper, ColdBootWithTheLidOpenSendsAStateReport) {
  Node n = sn002();
  n.sn002().lid_open = true;
  n.boot();
  n.run_for(TamperDebouncer::kDebounceMs + 20);
  const auto t = of(n.drain_and_ack(), EventKind::kTamper);
  ASSERT_EQ(t.size(), 1u);
  EXPECT_TRUE(t[0].flag);
  EXPECT_EQ(t[0].b, 1u);
}

// --- lamps ---------------------------------------------------------------------------------

TEST(Sn002Lamps, DarkOnBatteryEvenWithEventsAndFaults) {
  Node n = sn002();
  on_battery(n);
  settle(n);
  n.sn002().lid_open = true;
  n.imu(kVib).impact(1'500);
  n.run_for(200);
  for (std::size_t l = 0; l < kLampCount; ++l) EXPECT_FALSE(n.sn002().lamp(static_cast<Lamp>(l)));
  for (std::uint8_t s = 0; s < kSlots; ++s) EXPECT_FALSE(n.sn002().sensor_led(s));
}

TEST(Sn002Lamps, OnUsbEachRoleShowsItsState) {
  Node n = sn002();
  n.adc.mv = 3'300;
  settle(n);
  // Hub answered (drain_and_ack): LINK solid. Sensors running: their lamps solid.
  n.run_for(10);
  EXPECT_TRUE(n.sn002().lamp(Lamp::kLink));
  for (std::uint8_t s = 0; s < kSlots; ++s) EXPECT_TRUE(n.sn002().sensor_led(s)) << int(s);
  EXPECT_FALSE(n.sn002().lamp(Lamp::kTamper));
  EXPECT_FALSE(n.sn002().lamp(Lamp::kBattery));
  EXPECT_FALSE(n.sn002().lamp(Lamp::kFault));
  n.sn002().lid_open = true;
  n.run_for(TamperDebouncer::kDebounceMs + 20);
  EXPECT_TRUE(n.sn002().lamp(Lamp::kTamper));
  // An impact flashes VIB (inverted for SlotLedPolicy::kFlashMs).
  n.imu(kVib).impact(1'500);
  n.run_for(20);
  EXPECT_FALSE(n.sn002().sensor_led(kVib));
  n.run_for(SlotLedPolicy::kFlashMs + 10);
  EXPECT_TRUE(n.sn002().sensor_led(kVib));
}

TEST(Sn002Lamps, LinkBlinksWhileJoining) {
  Node n = sn002();
  n.boot();  // nothing ACKed: the hub has not answered
  std::vector<bool> seen;
  for (int i = 0; i < 6; ++i) {
    n.run_for(500);
    seen.push_back(n.sn002().lamp(Lamp::kLink));
  }
  EXPECT_NE(std::count(seen.begin(), seen.end(), true), 0);
  EXPECT_NE(std::count(seen.begin(), seen.end(), false), 0);
}

TEST(Sn002Lamps, ServiceModeFromBootLightsThemOnBattery) {
  Node n = sn002();
  on_battery(n);
  settle(n);
  n.gpio.set(b2::kBootButton, false);  // BOOT pressed (active low)
  n.run_for(100);
  n.gpio.set(b2::kBootButton, true);
  n.run_for(100);
  EXPECT_TRUE(n.runtime().service_mode());
  EXPECT_TRUE(n.sn002().lamp(Lamp::kLink));
  EXPECT_TRUE(n.sn002().sensor_led(kMag));
  n.run_for(SlotLedPolicy::kServiceMs);
  EXPECT_FALSE(n.sn002().lamp(Lamp::kLink));
  EXPECT_FALSE(n.sn002().sensor_led(kMag));
}

// --- USER button ---------------------------------------------------------------------------

TEST(Sn002User, ShortPressSendsATestReportAndBlinksEveryLampForTenSeconds) {
  Node n = sn002();
  on_battery(n, 3'000);
  settle(n);
  n.sn002().user_pressed = true;
  n.run_for(200);
  n.sn002().user_pressed = false;
  n.run_for(50);
  const auto ev = n.drain_and_ack();
  const auto u = of(ev, EventKind::kUserButton);
  ASSERT_EQ(u.size(), 1u);
  EXPECT_EQ(u[0].a, static_cast<std::int32_t>(UserButtonAction::kTestReport));
  EXPECT_EQ(u[0].priority, Priority::kP1State);
  const auto hb = of(ev, EventKind::kHeartbeat);
  ASSERT_EQ(hb.size(), 1u);  // the test report itself
  EXPECT_EQ(hb[0].a, 3'000);
  EXPECT_TRUE(n.runtime().lamp_test_active());
  // On battery, yet the lamps blink together.
  int lit = 0, dark = 0;
  for (int i = 0; i < 16; ++i) {
    n.run_for(125);
    const bool on = n.sn002().lamp(Lamp::kFault);
    EXPECT_EQ(n.sn002().lamp(Lamp::kLink), on);
    EXPECT_EQ(n.sn002().sensor_led(kEnv), on);
    (on ? lit : dark)++;
  }
  EXPECT_GT(lit, 0);
  EXPECT_GT(dark, 0);
  n.run_for(kLampTestMs);
  EXPECT_FALSE(n.runtime().lamp_test_active());
  EXPECT_FALSE(n.sn002().lamp(Lamp::kLink));  // back to dark on battery
}

TEST(Sn002User, LongPressIsOneCommissionRequestWhileHeld) {
  Node n = sn002();
  settle(n);
  n.sn002().user_pressed = true;
  n.run_for(UserButton::kLongPressMs + 100);
  auto u = of(n.drain_and_ack(), EventKind::kUserButton);
  ASSERT_EQ(u.size(), 1u);
  EXPECT_EQ(u[0].a, static_cast<std::int32_t>(UserButtonAction::kCommissionRequest));
  n.run_for(2'000);
  n.sn002().user_pressed = false;  // the release adds nothing
  n.run_for(100);
  EXPECT_TRUE(of(n.drain_and_ack(), EventKind::kUserButton).empty());
}

TEST(Sn002User, HeldThroughAColdBootAndGlitchesDoNothing) {
  Node n = sn002();
  n.sn002().user_pressed = true;
  n.boot();
  n.run_for(UserButton::kLongPressMs + 500);
  n.sn002().user_pressed = false;
  n.run_for(100);
  for (int i = 0; i < 5; ++i) {  // 10 ms glitches, shorter than the debounce
    n.sn002().user_pressed = true;
    n.run_for(10);
    n.sn002().user_pressed = false;
    n.run_for(100);
  }
  EXPECT_TRUE(of(n.drain_and_ack(), EventKind::kUserButton).empty());
}

// --- power: USB / battery, AA level --------------------------------------------------------

TEST(Sn002Power, UsbBatterySwitchoverFollowsUsbPresent) {
  Node n = sn002();
  n.adc.mv = 2'300;  // low cells: matters only on battery
  settle(n);
  EXPECT_EQ(n.platform().power.source(), PowerSource::kUsb);
  EXPECT_TRUE(n.runtime().external_power());
  EXPECT_EQ(n.runtime().power_mode(), PowerMode::kNormal);
  n.sn002().usb = false;
  n.run_for(20);
  EXPECT_EQ(n.platform().power.source(), PowerSource::kBatteryOnly);
  EXPECT_FALSE(n.runtime().external_power());
  EXPECT_FALSE(n.sn002().lamp(Lamp::kLink));  // dark on battery
  // The cells' last loaded reading (2.3 V) is above 2.2 V: still normal.
  EXPECT_EQ(n.runtime().power_mode(), PowerMode::kNormal);
  n.sn002().usb = true;
  n.run_for(20);
  EXPECT_TRUE(n.runtime().external_power());
  EXPECT_TRUE(n.sn002().lamp(Lamp::kLink));
}

TEST(Sn002Power, AaLowBatteryUnderLoadEntersLowPowerAndLightsBatt) {
  Node n = sn002();
  on_battery(n, 2'900);
  settle(n);
  EXPECT_EQ(n.runtime().power_mode(), PowerMode::kNormal);
  EXPECT_FALSE(n.runtime().battery_low());
  // The pair sags under radio bursts toward the end of life.
  n.adc.mv = 2'150;
  n.run_for(60'000 + 100);  // a heartbeat
  const auto ev = n.drain_and_ack();
  const auto pm = of(ev, EventKind::kPowerMode);
  ASSERT_EQ(pm.size(), 1u);
  EXPECT_EQ(pm[0].a, static_cast<std::int32_t>(PowerMode::kLowPower));
  const auto hb = of(ev, EventKind::kHeartbeat);
  ASSERT_FALSE(hb.empty());
  EXPECT_EQ(hb.back().a, 2'150);
  EXPECT_TRUE(n.runtime().battery_low());
  n.runtime().start_service_mode();
  EXPECT_TRUE(n.sn002().lamp(Lamp::kBattery));
  // Below 2.0 V loaded: critical.
  n.adc.mv = 1'950;
  n.run_for(4 * 60'000 + 100);
  EXPECT_EQ(n.runtime().power_mode(), PowerMode::kCritical);
}

TEST(Sn002Power, LoadedReadingIsTheLowestSinceTheLastHeartbeat) {
  Node n = sn002();
  on_battery(n, 3'000);
  settle(n);
  // One sag right after a send (a P0 makes the node transmit at once).
  n.adc.mv = 2'100;
  n.imu(kVib).impact(1'500);
  n.run_for(30);
  n.adc.mv = 3'000;  // the cells recover at rest
  n.run_for(60'000);
  const auto hb = of(n.drain_and_ack(), EventKind::kHeartbeat);
  ASSERT_FALSE(hb.empty());
  EXPECT_EQ(hb.back().a, 2'100) << "the resting voltage hides an empty lithium cell";
}

TEST(Sn002Power, OtaRefusedOnTiredCellsAllowedOnUsb) {
  Node n = sn002();
  on_battery(n, 2'500);  // below battery_ota_min_mv (2.6 V)
  settle(n);
  n.link.downlink.push_back(OtaOffer{100, 1024, {}});
  n.run_for(20);
  EXPECT_NE(n.runtime().ota().state, OtaState::kDownloading);
  n.sn002().usb = true;
  n.run_for(20);
  n.link.downlink.push_back(OtaOffer{100, 1024, {}});
  n.run_for(20);
  EXPECT_EQ(n.runtime().ota().state, OtaState::kDownloading);
}

TEST(Sn002Power, LiFeS2CurveIsMonotonic) {
  std::uint8_t last = 0;
  for (std::uint32_t mv = 1'800; mv <= 3'600; mv += 10) {
    const auto p = lifes2_pair_percent(mv);
    EXPECT_GE(p, last) << mv;
    last = p;
  }
  EXPECT_EQ(lifes2_pair_percent(0), 0);
}

// --- faulting sensors: quarantine = disable + report ---------------------------------------

TEST(Sn002Quarantine, AMissingBuiltInSensorIsAP0FaultAndRetriedWithBackoff) {
  Node n = sn002();
  ASSERT_TRUE(apply_fault(n, "tmag_missing"));
  n.boot();
  n.run_for(50);
  const auto f = of(n.drain_and_ack(), EventKind::kModuleFault);
  ASSERT_EQ(f.size(), 1u);
  EXPECT_EQ(f[0].slot, kMag);
  EXPECT_EQ(f[0].a, static_cast<std::int32_t>(ModuleFault::kInitFailed));
  EXPECT_EQ(f[0].priority, Priority::kP0Critical) << "door sensing is lost";
  EXPECT_FALSE(n.runtime().slot(kMag).enabled);
  EXPECT_TRUE(n.runtime().slot(kVib).enabled) << "the others keep working";
  // The part comes back (a cold joint that reflowed): retried after an hour.
  n.i2c.attach(I2cBus::kRoot, Tmag5273ContactDriver::kAddr, &n.hall(kMag));
  n.run_for(30 * 60'000);
  n.drain_and_ack();
  EXPECT_FALSE(n.runtime().slot(kMag).enabled);
  n.run_for(31 * 60'000 + 60'000);
  EXPECT_TRUE(n.runtime().slot(kMag).enabled);
}

TEST(Sn002Quarantine, AStuckPirIsDisabledItsRailCutAndRetriedByAPowerCycle) {
  Node n = sn002();
  ASSERT_TRUE(apply_fault(n, "pir_stuck"));
  n.boot();
  const auto f = of(run_acked(n, PirDriver::kPir223d.warm_max_ms + 1'000), EventKind::kModuleFault);
  ASSERT_FALSE(f.empty());
  EXPECT_EQ(f[0].slot, kPir);
  EXPECT_EQ(f[0].a, static_cast<std::int32_t>(ModuleFault::kWarmupTimeout));
  EXPECT_FALSE(n.runtime().slot(kPir).enabled);
  EXPECT_EQ(n.runtime().fixed_quarantined(), 1u << kPir);
  EXPECT_FALSE(n.sn002().pir_powered()) << "quarantine cuts PIR_PWR";
  EXPECT_TRUE(n.runtime().slot(kMag).enabled);
  // Fixed later: the retry power-cycles the rail and the PIR warms up again.
  n.sn002().pir_stuck_high = false;
  n.run_for(NodeRuntime::kFixedRetryMs + 60'000 + 100);
  EXPECT_TRUE(n.sn002().pir_powered());
  EXPECT_TRUE(n.runtime().slot(kPir).enabled);
  EXPECT_EQ(n.runtime().fixed_quarantined(), 0u);
}

TEST(Sn002Quarantine, FaultLampLightsOnUsb) {
  Node n = sn002();
  ASSERT_TRUE(apply_fault(n, "sht_missing"));
  n.boot();
  n.run_for(100);
  n.drain_and_ack();
  n.run_for(10);
  EXPECT_TRUE(n.sn002().lamp(Lamp::kFault));
  // The ENV lamp slow-blinks for its fault (SlotLedPolicy).
  std::vector<bool> seen;
  for (int i = 0; i < 4; ++i) {
    n.run_for(600);
    seen.push_back(n.sn002().sensor_led(kEnv));
  }
  EXPECT_NE(std::count(seen.begin(), seen.end(), true), 0);
  EXPECT_NE(std::count(seen.begin(), seen.end(), false), 0);
}

// --- red team 2026-11-21 (TL-SN-002) -------------------------------------------------------

// Every deep-sleep wake is a reset. The loaded minimum must survive it, or a
// sleepy node reports (and admits an OTA on) the resting voltage, which hides an
// empty Li-FeS2 pair.
TEST(Sn002RedTeam, LoadedBatteryLevelSurvivesADeepSleepWake) {
  Node n = sn002();
  on_battery(n, 3'000);
  settle(n);
  n.adc.mv = 2'100;  // sag right after a send
  n.imu(kVib).impact(1'500);
  n.run_for(30);
  n.drain_and_ack();
  n.adc.mv = 3'000;           // the pair recovers at rest
  n.wake_from_deep_sleep(0);  // timer wake: a reset
  ASSERT_TRUE(n.runtime().resumed_from_sleep());
  // An OTA offered after the wake is judged on the loaded level, not 3.0 V at rest.
  n.link.downlink.push_back(OtaOffer{100, 1024, {}});
  n.run_for(20);
  EXPECT_NE(n.runtime().ota().state, OtaState::kDownloading);
  n.run_for(60'000 + 100);
  const auto hb = of(n.drain_and_ack(), EventKind::kHeartbeat);
  ASSERT_FALSE(hb.empty());
  EXPECT_EQ(hb.front().a, 2'100);
  EXPECT_EQ(n.runtime().power_mode(), PowerMode::kLowPower);
}

// A node that wakes more often than the retry period still retries a
// quarantined built-in sensor on schedule (the schedule is kept in LP RAM).
TEST(Sn002RedTeam, QuarantinedSensorIsRetriedAcrossFrequentWakes) {
  Node n = sn002();
  ASSERT_TRUE(apply_fault(n, "pir_stuck"));
  n.boot();
  run_acked(n, PirDriver::kPir223d.warm_max_ms + 1'000);
  ASSERT_EQ(n.runtime().fixed_quarantined(), 1u << kPir);
  n.sn002().pir_stuck_high = false;
  for (int i = 0; i < 5; ++i) {  // 100 min of 20-min sleeps
    n.run_for(20 * 60'000);
    n.drain_and_ack();
    n.wake_from_deep_sleep(0);
    ASSERT_TRUE(n.runtime().resumed_from_sleep());
  }
  n.run_for(2 * 60'000);
  EXPECT_TRUE(n.runtime().slot(kPir).enabled) << "retried after 1 h despite the wakes";
  EXPECT_EQ(n.runtime().fixed_quarantined(), 0u);
}

// Backoff doubles per failed retry (1 h, 2 h, 4 h ...), it does not quadruple.
TEST(Sn002RedTeam, BuiltInRetryBackoffDoubles) {
  Node n = sn002();
  ASSERT_TRUE(apply_fault(n, "tmag_missing"));
  n.boot();
  std::vector<TimeMs> faults;
  for (int m = 0; m < 3 * 60 + 30; ++m) {  // faults at 0, ~1 h and ~3 h
    n.run_for(60'000);
    for (const auto& e : n.drain_and_ack()) {
      if (e.kind == EventKind::kModuleFault && e.slot == kMag) faults.push_back(e.at_ms);
    }
  }
  ASSERT_GE(faults.size(), 3u);
  const auto h = [](TimeMs ms) { return (ms + 30 * 60'000) / (60 * 60'000); };  // nearest hour
  EXPECT_EQ(h(faults[1] - faults[0]), 1u);
  EXPECT_EQ(h(faults[2] - faults[1]), 2u);
}

// RT-08 on TL-SN-002: someone moving through the whole PIR warm-up makes it time
// out while toggling. The motion is reported and the PIR keeps working: it must
// not be quarantined (and powered off) for an hour.
TEST(Sn002RedTeam, MotionThroughThePirWarmUpDoesNotQuarantineIt) {
  Node n = sn002();
  n.boot();
  std::vector<DeviceEvent> ev;
  bool level = false;
  for (TimeMs t = 0; t < PirDriver::kPir223d.warm_max_ms + 5'000; t += 700) {
    level = !level;
    n.set_line(b2::kPirOut, level);
    n.run_for(700);
    for (const auto& e : n.drain_and_ack()) ev.push_back(e);
  }
  EXPECT_FALSE(of(ev, EventKind::kMotion).empty()) << "the motion is reported";
  EXPECT_TRUE(n.runtime().slot(kPir).enabled);
  EXPECT_EQ(n.runtime().fixed_quarantined(), 0u);
  EXPECT_TRUE(n.sn002().pir_powered());
}

// The open lid is masked as a wake source; the node must still look at it, so a
// close and re-open between heartbeats is seen (maintenance ends, lid closed,
// then a thief opens it before the next heartbeat).
TEST(Sn002RedTeam, OpenLidIsPolledWhileItsWakeIsMasked) {
  Node n = sn002();
  on_battery(n, 3'000);
  settle(n);
  n.sn002().lid_open = true;
  n.run_for(TamperDebouncer::kDebounceMs + 20);
  n.drain_and_ack();
  ASSERT_FALSE(n.sleep.wake_enabled(b2::kTamper));
  // Idle for a while (nothing else due soon): the node still plans to look.
  for (int i = 0; i < 5; ++i) {
    n.run_for(7'000);
    n.drain_and_ack();
    EXPECT_LE(n.runtime().next_wake_ms(), n.clock.now_ms() + NodeRuntime::kMaskedPollMs) << i;
  }
  // Also after a deep-sleep wake that restored "open" from LP RAM.
  n.wake_from_deep_sleep(0);
  n.run_for(7'000);
  n.drain_and_ack();
  EXPECT_LE(n.runtime().next_wake_ms(), n.clock.now_ms() + NodeRuntime::kMaskedPollMs);
  // Closed again: no polling, the line is a wake source once more.
  n.sn002().lid_open = false;
  n.run_for(TamperDebouncer::kDebounceMs + 20);
  EXPECT_TRUE(n.sleep.wake_enabled(b2::kTamper));
}

// A jammed, bouncing or abused USER button: one test report per cooldown.
TEST(Sn002RedTeam, UserPressesAreRateLimited) {
  Node n = sn002();
  on_battery(n, 3'000);
  settle(n);
  auto press = [&](TimeMs hold) {
    n.sn002().user_pressed = true;
    n.run_for(hold);
    n.sn002().user_pressed = false;
    n.run_for(200);
  };
  std::vector<DeviceEvent> ev;
  for (int i = 0; i < 10; ++i) {
    press(150);
    for (const auto& e : n.drain_and_ack()) ev.push_back(e);
  }
  EXPECT_EQ(of(ev, EventKind::kUserButton).size(), 1u);
  EXPECT_EQ(of(ev, EventKind::kHeartbeat).size(), 1u);
  n.run_for(NodeRuntime::kUserCooldownMs);
  n.drain_and_ack();
  press(150);
  EXPECT_EQ(of(n.drain_and_ack(), EventKind::kUserButton).size(), 1u) << "after the cooldown";
  // A commissioning request has its own cooldown: a long press right after counts.
  press(UserButton::kLongPressMs + 100);
  const auto u = of(n.drain_and_ack(), EventKind::kUserButton);
  ASSERT_EQ(u.size(), 1u);
  EXPECT_EQ(u[0].a, static_cast<std::int32_t>(UserButtonAction::kCommissionRequest));
}

// A loaded reading hovering at the threshold must not flap the mode (and the
// optional loads) every heartbeat.
TEST(Sn002RedTeam, PowerModeHasHysteresis) {
  Node n = sn002();
  on_battery(n, 2'150);
  settle(n);
  n.run_for(60'000 + 100);
  ASSERT_EQ(n.runtime().power_mode(), PowerMode::kLowPower);
  n.drain_and_ack();
  n.adc.mv = 2'250;  // above 2.2 V, inside the 100 mV band
  n.run_for(8 * 60'000);
  EXPECT_EQ(n.runtime().power_mode(), PowerMode::kLowPower);
  EXPECT_TRUE(of(n.drain_and_ack(), EventKind::kPowerMode).empty());
  n.adc.mv = 2'350;
  n.run_for(8 * 60'000);
  EXPECT_EQ(n.runtime().power_mode(), PowerMode::kNormal);
}

}  // namespace
}  // namespace talos::fw::sim
