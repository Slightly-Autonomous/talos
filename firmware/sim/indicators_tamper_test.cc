// TL-SN-001 Rev A additions on the virtual board: EXP_C (slot LEDs, tamper,
// unused pins), EXP_WAKE (active high, shares the ext1 mask with the slot WAKE
// lines), the status LED blink codes and the slot LED policy.
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "firmware/sim/virtual_board.h"

namespace talos::fw::sim {
namespace {

constexpr std::uint8_t kExpWake = 14;
constexpr std::uint8_t kStatusLed = 8;

Node rev_a() { return Node(tl_sn_001_board()); }

void on_battery(Node& n) {
  n.rev_a().charger.vin_good = false;
  n.rev_a().source = RevABoard::Source::kBatteryOnly;
}

std::vector<DeviceEvent> tamper_events(Node& n) {
  n.run_for(5);
  std::vector<DeviceEvent> out;
  for (const auto& e : n.drain_and_ack()) {
    if (e.kind == EventKind::kTamper) out.push_back(e);
  }
  return out;
}

// --- EXP_C ------------------------------------------------------------------------------

TEST(ExpC, BootDrivesUnusedPinsLowLedsDarkAndLeavesTamperAnInput) {
  Node n = rev_a();
  n.boot();
  const Tca9535Model& c = n.rev_a().exp_c;
  for (std::uint8_t pin = 0; pin < 4; ++pin) {
    EXPECT_TRUE(c.is_output(pin)) << int(pin);
    EXPECT_TRUE(c.level(pin)) << int(pin);  // active low: dark
  }
  EXPECT_FALSE(c.is_output(4));  // TAMPER_N
  for (std::uint8_t pin = 5; pin < 16; ++pin) {
    EXPECT_TRUE(c.is_output(pin)) << int(pin);
    EXPECT_FALSE(c.level(pin)) << int(pin);
  }
  // Output latch written before direction: no unused pin ever drove high.
  EXPECT_EQ(c.ever_driven_high & 0xFFE0, 0);
  EXPECT_TRUE(n.runtime().board_ok());
}

TEST(ExpC, PresenceProbeDoesNotLeaveTheInterruptAsserted) {
  Node n = rev_a();
  n.insert(1, ModuleCode::kSht40);
  n.boot();
  EXPECT_FALSE(n.rev_a().exp_wake());
  n.run_for(NodeRuntime::kMinIntervalMs * 7);  // heartbeats probe PRESENT_N again
  EXPECT_FALSE(n.rev_a().exp_wake());
  EXPECT_FALSE(n.runtime().expander_wake_masked());
}

// --- tamper -----------------------------------------------------------------------------

TEST(Tamper, LidOpenIsADebouncedP0AndClosingRestoresAtP1) {
  Node n = rev_a();
  n.boot();
  EXPECT_EQ(n.runtime().tamper_open(), std::nullopt);  // known once it held kDebounceMs
  n.run_for(TamperDebouncer::kDebounceMs + 10);
  EXPECT_EQ(n.runtime().tamper_open(), std::optional<bool>{false});
  auto report = tamper_events(n);  // closed at boot: a P1 state report
  ASSERT_EQ(report.size(), 1u);
  EXPECT_FALSE(report[0].flag);
  EXPECT_EQ(report[0].b, 1u);
  EXPECT_EQ(report[0].priority, Priority::kP1State);
  n.rev_a().lid_open = true;
  EXPECT_TRUE(n.gpio.read(kExpWake));  // TAMPER_N change pulls EXP_INT_N low: EXP_WAKE high
  n.run_for(TamperDebouncer::kDebounceMs - 10);
  EXPECT_EQ(n.runtime().tamper_open(), std::optional<bool>{false});  // still debouncing
  EXPECT_FALSE(n.rev_a().exp_wake());                                // INT released by the read
  n.run_for(20);
  auto open = tamper_events(n);
  ASSERT_EQ(open.size(), 1u);
  EXPECT_TRUE(open[0].flag);
  EXPECT_EQ(open[0].priority, Priority::kP0Critical);
  EXPECT_EQ(open[0].slot, kNoSlot);
  EXPECT_EQ(open[0].b, 0u);  // a transition
  n.rev_a().lid_open = false;
  n.run_for(TamperDebouncer::kDebounceMs + 20);
  auto closed = tamper_events(n);
  ASSERT_EQ(closed.size(), 1u);
  EXPECT_FALSE(closed[0].flag);
  EXPECT_EQ(closed[0].priority, Priority::kP1State);
}

TEST(Tamper, BounceShorterThanTheDebounceIsIgnored) {
  Node n = rev_a();
  n.boot();
  n.run_for(100);
  tamper_events(n);  // the boot state report
  n.rev_a().lid_open = true;
  n.run_for(TamperDebouncer::kDebounceMs / 2);
  n.rev_a().lid_open = false;
  n.run_for(TamperDebouncer::kDebounceMs * 4);
  EXPECT_TRUE(tamper_events(n).empty());
  EXPECT_EQ(n.runtime().tamper_open(), std::optional<bool>{false});
}

// Red team: open/close chatter faster than the debounce must not keep an open
// lid unreported on the node either (INT on every edge, timed re-reads).
TEST(Tamper, ChatterFasterThanTheDebounceIsReportedOnTheNode) {
  Node n = rev_a();
  n.boot();
  n.run_for(100);
  tamper_events(n);  // the boot state report
  for (int i = 0; i < 20; ++i) {
    n.rev_a().lid_open = true;
    n.run_for(TamperDebouncer::kDebounceMs * 3 / 5);
    n.rev_a().lid_open = false;
    n.run_for(TamperDebouncer::kDebounceMs / 5);
  }
  n.rev_a().lid_open = true;
  n.run_for(10);
  auto ev = tamper_events(n);
  ASSERT_FALSE(ev.empty());
  EXPECT_TRUE(ev[0].flag);
  EXPECT_EQ(ev[0].priority, Priority::kP0Critical);
}

TEST(Tamper, ColdBootWithTheLidOpenReportsTheState) {
  Node n = rev_a();
  n.rev_a().lid_open = true;  // or the loop cut before power-up
  n.boot();
  n.run_for(TamperDebouncer::kDebounceMs + 20);
  auto ev = tamper_events(n);
  ASSERT_EQ(ev.size(), 1u);
  EXPECT_TRUE(ev[0].flag);
  EXPECT_EQ(ev[0].b, 1u);  // a state report
  EXPECT_EQ(ev[0].priority, Priority::kP0Critical);
}

TEST(Tamper, LidOpenedDuringDeepSleepWakesTheNodeAndIsReported) {
  Node n = rev_a();
  n.insert(0, ModuleCode::kTlMod002Pir);
  n.boot();
  n.run_for(1'000);
  n.drain_and_ack();
  n.run_for(10);
  // EXP_WAKE is armed next to the slot WAKE lines (one ext1 ANY_HIGH mask).
  EXPECT_TRUE(n.sleep.wake_enabled(kExpWake));
  EXPECT_TRUE(n.sleep.wake_enabled(n.board.slots[0].wake_line));
  // Asleep: the lid opens. The expander's INT asserts and EXP_WAKE goes high.
  n.rev_a().lid_open = true;
  ASSERT_TRUE(n.rev_a().exp_wake());
  n.wake_from_deep_sleep(1u << kExpWake);
  EXPECT_TRUE(n.runtime().resumed_from_sleep());
  EXPECT_FALSE(n.rev_a().exp_wake());  // boot read the ports: INT released
  n.run_for(TamperDebouncer::kDebounceMs + 20);
  auto ev = tamper_events(n);
  ASSERT_EQ(ev.size(), 1u);
  EXPECT_TRUE(ev[0].flag);
  EXPECT_EQ(ev[0].b, 0u);  // the state before the sleep was closed: a transition
  EXPECT_EQ(ev[0].priority, Priority::kP0Critical);
  // A later wake for something else does not report the lid again.
  n.wake_from_deep_sleep(0);
  n.run_for(TamperDebouncer::kDebounceMs + 20);
  EXPECT_TRUE(tamper_events(n).empty());
  EXPECT_EQ(n.runtime().tamper_open(), std::optional<bool>{true});
}

TEST(Tamper, OpenLidAlarmSurvivesAResetBeforeTheHubAcks) {
  Node n = rev_a();
  n.boot();
  n.run_for(100);
  n.link.connected = false;
  n.rev_a().lid_open = true;
  n.run_for(TamperDebouncer::kDebounceMs + 20);
  n.boot();  // power glitch, watchdog: the P0 ring replays it
  n.link.connected = true;
  n.run_for(10);
  int opened = 0;
  for (const auto& e : n.drain_and_ack()) {
    if (e.kind == EventKind::kTamper && e.flag && e.b == 0) ++opened;
  }
  EXPECT_EQ(opened, 1);
}

// --- EXP_WAKE ---------------------------------------------------------------------------

TEST(ExpWake, AStuckLineIsMaskedAsAWakeSourceAndReArmedWhenItFalls) {
  Node n = rev_a();
  n.boot();
  n.rev_a().exp_int_stuck = true;
  n.run_for(NodeRuntime::kWakeMaskMs + 100);
  EXPECT_TRUE(n.runtime().expander_wake_masked());
  EXPECT_FALSE(n.sleep.wake_enabled(kExpWake));
  // Masked survives a deep sleep (it would re-wake the node forever).
  n.wake_from_deep_sleep(0);
  EXPECT_TRUE(n.runtime().expander_wake_masked());
  EXPECT_FALSE(n.sleep.wake_enabled(kExpWake));
  // Tamper is still caught while masked: polled every kMaskedPollMs.
  n.rev_a().lid_open = true;
  n.run_for(NodeRuntime::kMaskedPollMs * 2 + TamperDebouncer::kDebounceMs);
  EXPECT_EQ(n.runtime().tamper_open(), std::optional<bool>{true});
  n.rev_a().exp_int_stuck = false;
  n.run_for(NodeRuntime::kMaskedPollMs * 2);
  EXPECT_FALSE(n.runtime().expander_wake_masked());
  EXPECT_TRUE(n.sleep.wake_enabled(kExpWake));
}

TEST(ExpWake, UsbPlugRaisesTheInterruptAndUpdatesExternalPower) {
  Node n = rev_a();
  on_battery(n);
  n.boot();
  EXPECT_FALSE(n.runtime().external_power());
  n.rev_a().charger.vin_good = true;  // CHG_PG_N falls
  n.rev_a().source = RevABoard::Source::kUsb;
  EXPECT_TRUE(n.rev_a().exp_wake());
  n.run_for(5);
  EXPECT_TRUE(n.runtime().external_power());
  EXPECT_FALSE(n.rev_a().exp_wake());
}

// --- slot LEDs (L-03) -------------------------------------------------------------------

TEST(SlotLeds, DarkOnBatteryEvenWithCardsAndWakeEvents) {
  Node n = rev_a();
  on_battery(n);
  n.insert(0, ModuleCode::kSht40);
  n.insert(2, ModuleCode::kLis2dw12);
  n.boot();
  ASSERT_TRUE(n.runtime().slot(0).enabled);
  n.imu(2).impact(900);
  for (int i = 0; i < 50; ++i) {
    n.run_for(5);
    for (std::uint8_t s = 0; s < kSlots; ++s) ASSERT_FALSE(n.rev_a().slot_led(s)) << int(s);
  }
  EXPECT_EQ(n.rev_a().exp_c.ever_driven_high & 0x000F, 0x000F);  // only ever driven dark
}

TEST(SlotLeds, ServiceModeLightsThemOnBatteryForTenMinutes) {
  Node n = rev_a();
  on_battery(n);
  n.insert(0, ModuleCode::kSht40);
  n.boot();
  n.runtime().start_service_mode();
  n.run_for(5);
  EXPECT_TRUE(n.rev_a().slot_led(0));   // solid: enumerated
  EXPECT_FALSE(n.rev_a().slot_led(1));  // empty slot stays dark
  n.run_for(SlotLedPolicy::kServiceMs);
  EXPECT_FALSE(n.rev_a().slot_led(0));
  EXPECT_FALSE(n.runtime().service_mode());
}

TEST(SlotLeds, OnUsbSolidSlowBlinkAndWakeFlash) {
  Node n = rev_a();
  n.insert(0, ModuleCode::kSht40);
  n.insert(1, ModuleCode::kTlMod004Env);
  n.id(1).blank();  // fault: ID never programmed
  n.insert(2, ModuleCode::kLis2dw12);
  n.boot();
  n.run_for(5);
  EXPECT_TRUE(n.runtime().external_power());
  EXPECT_TRUE(n.rev_a().slot_led(0));
  EXPECT_TRUE(n.rev_a().slot_led(2));
  // Slot 1 blinks slowly: on for half of each 2 s period.
  int lit = 0;
  for (int i = 0; i < 400; ++i) {
    n.run_for(10);
    lit += n.rev_a().slot_led(1) ? 1 : 0;
  }
  EXPECT_NEAR(lit, 200, 4);
  // A wake event from slot 2: its solid LED blinks off briefly.
  n.imu(2).impact(900);
  bool went_dark = false;
  for (int i = 0; i < 20 && !went_dark; ++i) {
    n.run_for(5);
    went_dark = !n.rev_a().slot_led(2);
  }
  EXPECT_TRUE(went_dark);
  n.run_for(SlotLedPolicy::kFlashMs + 20);
  EXPECT_TRUE(n.rev_a().slot_led(2));
}

TEST(SlotLeds, ALitLedDoesNotStopADeepSleepResume) {
  Node n = rev_a();
  n.insert(0, ModuleCode::kTlMod002Pir);  // a wake card: kept powered across the sleep
  n.boot();
  n.run_for(5);
  ASSERT_TRUE(n.rev_a().slot_led(0));
  const auto cuts = n.rev_a().aon_cuts[0];
  n.wake_from_deep_sleep(0);
  EXPECT_TRUE(n.runtime().resumed_from_sleep());
  EXPECT_EQ(n.rev_a().aon_cuts[0], cuts);  // the card kept its rail
  n.run_for(5);
  EXPECT_TRUE(n.rev_a().slot_led(0));
}

// --- status LED (L-01, L-04) ------------------------------------------------------------

// Milliseconds the status LED (GPIO8, active low) is lit over `ms`.
TimeMs status_on_ms(Node& n, TimeMs ms) {
  TimeMs on = 0;
  for (TimeMs t = 0; t < ms; ++t) {
    n.run_for(1);
    on += n.gpio.driven[kStatusLed] ? 0 : 1;
  }
  return on;
}

TEST(StatusLed, BootThenJoiningThenChargingOnUsb) {
  Node n = rev_a();
  n.boot();
  EXPECT_EQ(n.runtime().status_code(), StatusCode::kBoot);
  EXPECT_FALSE(n.gpio.driven[kStatusLed]);  // first pulse at once
  n.run_for(kBootShowMs);
  EXPECT_EQ(n.runtime().status_code(), StatusCode::kJoining);  // the hub has not answered
  n.drain_and_ack();
  n.run_for(5);
  EXPECT_EQ(n.runtime().status_code(), StatusCode::kCharging);  // joined, on USB
  const TimeMs on = status_on_ms(n, 8'000);
  EXPECT_EQ(on, 4 * kStatusPulseMs);  // two periods of two 20 ms pulses
}

TEST(StatusLed, JoinedOnBatteryIsOnePulseEveryTenSeconds) {
  Node n = rev_a();
  on_battery(n);
  n.boot();
  n.run_for(kBootShowMs);
  n.drain_and_ack();
  n.run_for(5);
  EXPECT_EQ(n.runtime().status_code(), StatusCode::kJoined);
  EXPECT_EQ(status_on_ms(n, 20'000), 2 * kStatusPulseMs);
}

TEST(StatusLed, CardErrorAndFault) {
  Node n = rev_a();
  n.insert(1, ModuleCode::kTlMod004Env);
  n.id(1).blank();
  n.boot();
  n.run_for(kBootShowMs + 10);
  EXPECT_EQ(n.runtime().status_code(), StatusCode::kCardError);
  Node m = rev_a();
  m.rev_a().exp_c.nack = true;  // EXP_C dead: the board did not take its safe state
  m.boot();
  EXPECT_FALSE(m.runtime().board_ok());
  EXPECT_EQ(m.runtime().status_code(), StatusCode::kFault);
}

TEST(StatusLed, SameScriptSameLedTrace) {
  // Determinism: LED levels come from the injected clock only.
  std::vector<bool> trace[2];
  for (auto& t : trace) {
    Node n = rev_a();
    n.insert(0, ModuleCode::kSht40);
    n.boot();
    for (int i = 0; i < 6'000; ++i) {
      n.run_for(1);
      if (i == 3'500) n.drain_and_ack();
      t.push_back(n.gpio.driven[kStatusLed]);
      t.push_back(n.rev_a().slot_led(0));
    }
  }
  EXPECT_EQ(trace[0], trace[1]);
}

// --- service button (BOOT, GPIO9) ------------------------------------------------------

constexpr std::uint8_t kBoot = 9;

void press(Node& n, TimeMs ms) {
  n.gpio.set(kBoot, false);  // active low
  n.run_for(ms);
  n.gpio.set(kBoot, true);
  n.run_for(50);
}

TEST(ServiceButton, APressTogglesServiceModeWithoutALaptop) {
  Node n = rev_a();
  on_battery(n);
  n.insert(0, ModuleCode::kSht40);
  n.boot();
  EXPECT_FALSE(n.gpio.is_output(kBoot));  // a strap pin: never driven
  n.run_for(100);
  EXPECT_FALSE(n.rev_a().slot_led(0));
  press(n, 100);
  EXPECT_TRUE(n.runtime().service_mode());
  EXPECT_TRUE(n.rev_a().slot_led(0));
  press(n, 100);  // second press ends it
  EXPECT_FALSE(n.runtime().service_mode());
  EXPECT_FALSE(n.rev_a().slot_led(0));
  press(n, 100);
  n.run_for(SlotLedPolicy::kServiceMs);  // or the timeout does
  EXPECT_FALSE(n.runtime().service_mode());
  EXPECT_FALSE(n.rev_a().slot_led(0));
}

TEST(ServiceButton, GlitchesAndAButtonHeldThroughBootAreIgnored) {
  Node n = rev_a();
  on_battery(n);
  n.boot();
  n.run_for(100);
  press(n, 10);  // shorter than the 30 ms debounce
  EXPECT_FALSE(n.runtime().service_mode());
  Node m = rev_a();
  on_battery(m);
  m.gpio.set(kBoot, false);  // held while the app starts (after the ROM sampled the strap)
  m.boot();
  m.run_for(500);
  EXPECT_FALSE(m.runtime().service_mode());
  m.gpio.set(kBoot, true);
  m.run_for(100);
  EXPECT_FALSE(m.runtime().service_mode());  // releasing is not a press
}

}  // namespace
}  // namespace talos::fw::sim
