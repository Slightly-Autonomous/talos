// Pure indicator and tamper state machines (L-01, L-03, L-04, HD-032).
#include "firmware/portable/indicators.h"

#include <gtest/gtest.h>

namespace talos::fw {
namespace {

constexpr StatusCode kAll[] = {
    StatusCode::kJoined, StatusCode::kCharging,  StatusCode::kJoining, StatusCode::kLowBattery,
    StatusCode::kOta,    StatusCode::kCardError, StatusCode::kBoot,    StatusCode::kFault};

// Milliseconds on within one period, sampled at 1 ms.
TimeMs on_ms(StatusCode c) {
  TimeMs on = 0;
  for (TimeMs t = 0; t < blink_code(c).period_ms; ++t) on += status_led_on(c, t) ? 1 : 0;
  return on;
}

TEST(StatusLed, EveryCodeIsShortPulsesOnly) {
  for (StatusCode c : kAll) {
    const BlinkCode b = blink_code(c);
    EXPECT_EQ(on_ms(c), TimeMs{b.pulses} * kStatusPulseMs) << int(c);
    // No pulse longer than 20 ms, and pulses fit inside the period.
    EXPECT_LE(TimeMs{b.pulses} * (kStatusPulseMs + b.gap_ms), b.period_ms) << int(c);
  }
  EXPECT_EQ(on_ms(StatusCode::kOff), 0u);
}

TEST(StatusLed, SteadyStateDutyIsNegligible) {
  // Codes that can last for days: at most 2 % duty (a few uA average from a ~1-2 mA LED).
  for (StatusCode c : {StatusCode::kJoined, StatusCode::kCharging, StatusCode::kJoining,
                       StatusCode::kLowBattery, StatusCode::kCardError, StatusCode::kFault}) {
    EXPECT_LE(on_ms(c) * 100, TimeMs{blink_code(c).period_ms} * 2) << int(c);
  }
  EXPECT_LE(on_ms(StatusCode::kJoined) * 1000,
            TimeMs{blink_code(StatusCode::kJoined).period_ms} * 2);
}

TEST(StatusLed, CodesAreDistinguishable) {
  for (StatusCode a : kAll) {
    for (StatusCode b : kAll) {
      if (a == b) continue;
      const BlinkCode x = blink_code(a), y = blink_code(b);
      EXPECT_FALSE(x.pulses == y.pulses && x.period_ms == y.period_ms) << int(a) << " " << int(b);
    }
  }
}

TEST(StatusLed, NextEdgeWalksEveryTransition) {
  for (StatusCode c : kAll) {
    TimeMs t = 0;
    bool level = status_led_on(c, 0);
    int edges = 0;
    while (t < blink_code(c).period_ms) {
      const TimeMs next = status_next_edge(c, t);
      ASSERT_GT(next, t);
      for (TimeMs u = t + 1; u < next; ++u)
        ASSERT_EQ(status_led_on(c, u), level) << int(c) << " " << u;
      level = status_led_on(c, next);
      t = next;
      ++edges;
    }
    EXPECT_EQ(edges, 2 * blink_code(c).pulses) << int(c);
  }
}

TEST(StatusLed, PriorityOrder) {
  StatusInputs in;
  in.since_boot_ms = 10'000;
  EXPECT_EQ(select_status(in), StatusCode::kJoining);  // never heard the hub
  in.hub_heard = true;
  EXPECT_EQ(select_status(in), StatusCode::kJoined);
  in.external_power = true;  // L-04: charging shows only on external power
  EXPECT_EQ(select_status(in), StatusCode::kCharging);
  in.link_failing = true;
  EXPECT_EQ(select_status(in), StatusCode::kJoining);
  in.low_battery = true;
  EXPECT_EQ(select_status(in), StatusCode::kLowBattery);
  in.ota_active = true;
  EXPECT_EQ(select_status(in), StatusCode::kOta);
  in.card_error = true;
  EXPECT_EQ(select_status(in), StatusCode::kCardError);
  in.since_boot_ms = 100;
  EXPECT_EQ(select_status(in), StatusCode::kBoot);
  in.cold_boot = false;  // a deep-sleep wake does not replay the boot code
  EXPECT_EQ(select_status(in), StatusCode::kCardError);
  in.fault = true;
  EXPECT_EQ(select_status(in), StatusCode::kFault);
}

// --- slot LEDs --------------------------------------------------------------------------

TEST(SlotLeds, DarkOnBatteryWhateverTheSlotState) {
  SlotLedPolicy p;
  for (TimeMs t = 0; t < 5'000; t += 7) {
    for (SlotLedMode m : {SlotLedMode::kOff, SlotLedMode::kSolid, SlotLedMode::kSlowBlink}) {
      EXPECT_FALSE(p.led_on(0, m, t, false));
    }
  }
  p.note_wake(1, 100);  // a wake on battery never flashes: that would tell an intruder he was seen
  EXPECT_FALSE(p.led_on(1, SlotLedMode::kOff, 120, false));
  EXPECT_FALSE(p.led_on(1, SlotLedMode::kSolid, 120, false));
}

TEST(SlotLeds, MeaningsOnExternalPower) {
  SlotLedPolicy p;
  EXPECT_TRUE(p.led_on(0, SlotLedMode::kSolid, 1'234, true));
  EXPECT_FALSE(p.led_on(0, SlotLedMode::kOff, 1'234, true));
  EXPECT_TRUE(p.led_on(0, SlotLedMode::kSlowBlink, 10'000, true));   // first half of a 2 s period
  EXPECT_FALSE(p.led_on(0, SlotLedMode::kSlowBlink, 11'000, true));  // second half
  p.note_wake(2, 5'000);
  EXPECT_FALSE(p.led_on(2, SlotLedMode::kSolid, 5'000, true));  // solid LED blinks off
  EXPECT_TRUE(p.led_on(2, SlotLedMode::kOff, 5'149, true));     // dark LED blinks on
  EXPECT_TRUE(p.led_on(2, SlotLedMode::kSolid, 5'150, true));   // flash over
  EXPECT_TRUE(p.led_on(1, SlotLedMode::kSolid, 5'000, true));   // other slots unaffected
}

TEST(SlotLeds, ServiceModeLightsThemOnBatteryThenTimesOut) {
  SlotLedPolicy p;
  p.start_service(1'000);
  EXPECT_TRUE(p.led_on(3, SlotLedMode::kSolid, 1'000, false));
  EXPECT_TRUE(p.led_on(3, SlotLedMode::kSolid, 1'000 + SlotLedPolicy::kServiceMs - 1, false));
  EXPECT_FALSE(p.led_on(3, SlotLedMode::kSolid, 1'000 + SlotLedPolicy::kServiceMs, false));
  p.start_service(2'000);
  p.stop_service();
  EXPECT_FALSE(p.led_on(3, SlotLedMode::kSolid, 2'001, false));
}

TEST(SlotLeds, NextChangeCoversBlinkFlashAndServiceEnd) {
  SlotLedPolicy p;
  std::array<SlotLedMode, kSlots> modes{SlotLedMode::kSolid, SlotLedMode::kOff, SlotLedMode::kOff,
                                        SlotLedMode::kOff};
  EXPECT_EQ(p.next_change(modes, 500, false), ~TimeMs{0});  // dark and staying dark
  EXPECT_EQ(p.next_change(modes, 500, true), ~TimeMs{0});   // solid: nothing to wake for
  modes[1] = SlotLedMode::kSlowBlink;
  EXPECT_EQ(p.next_change(modes, 500, true), 1'000u);
  EXPECT_EQ(p.next_change(modes, 1'500, true), 2'000u);
  p.note_wake(0, 1'900);
  EXPECT_EQ(p.next_change(modes, 1'900, true), 2'000u);
  EXPECT_EQ(p.next_change(modes, 2'000, true), 2'050u);  // flash end
  p.start_service(10'000);
  EXPECT_EQ(p.next_change(modes, 10'000 + SlotLedPolicy::kServiceMs - 10, false),
            10'000 + SlotLedPolicy::kServiceMs);
}

// --- tamper -----------------------------------------------------------------------------

TEST(Tamper, LevelMustHoldForTheDebounceTime) {
  TamperDebouncer d;
  EXPECT_FALSE(d.sample(false, 0));
  EXPECT_FALSE(d.state());
  EXPECT_TRUE(d.pending());
  EXPECT_EQ(d.due_ms(), TamperDebouncer::kDebounceMs);
  EXPECT_FALSE(d.sample(false, TamperDebouncer::kDebounceMs - 1));
  EXPECT_TRUE(d.sample(false, TamperDebouncer::kDebounceMs));
  EXPECT_EQ(d.state(), std::optional<bool>{false});
  EXPECT_FALSE(d.pending());
}

TEST(Tamper, BouncesRestartTheTimerAndFailedReadsAreIgnored) {
  TamperDebouncer d;
  d.restore(false);
  EXPECT_FALSE(d.sample(true, 100));
  EXPECT_FALSE(d.sample(false, 120));  // bounced back: cancelled only once closed holds
  EXPECT_TRUE(d.pending());
  EXPECT_EQ(d.due_ms(), 120u + TamperDebouncer::kDebounceMs);
  EXPECT_FALSE(d.sample(false, 170));  // closed held: a knock, nothing happened
  EXPECT_FALSE(d.pending());
  EXPECT_EQ(d.state(), std::optional<bool>{false});
  EXPECT_FALSE(d.sample(true, 130 + 100));
  EXPECT_FALSE(d.sample(std::nullopt, 275));  // expander did not answer: proves nothing
  EXPECT_TRUE(d.pending());
  EXPECT_TRUE(d.sample(true, 280));
  EXPECT_EQ(d.state(), std::optional<bool>{true});
}

// Red team: a loop chattered open/closed faster than the debounce (a reed
// pulsed by a magnet) used to restart the timer forever and never report.
// Short closures no longer cancel the opening.
TEST(Tamper, ChatterFasterThanTheDebounceIsStillReportedOpen) {
  TamperDebouncer d;
  d.restore(false);
  bool reported = false;
  TimeMs at = 0;
  for (TimeMs t = 1'000; t < 2'000 && !reported; t += 20) {
    // 15 ms open, 5 ms closed, repeated.
    if (d.sample(true, t)) reported = true, at = t;
    if (!reported && d.sample(false, t + 15)) ADD_FAILURE() << "closed reported at " << t;
  }
  ASSERT_TRUE(reported);
  EXPECT_EQ(d.state(), std::optional<bool>{true});
  EXPECT_LE(at, 1'000u + TamperDebouncer::kDebounceMs + 20);
}

TEST(Tamper, ChatteredRestoreIsDelayedNeverEarly) {
  TamperDebouncer d;
  d.restore(true);
  for (TimeMs t = 0; t < 1'000; t += 20) {
    EXPECT_FALSE(d.sample(false, t));      // closed 15 ms ...
    EXPECT_FALSE(d.sample(true, t + 15));  // ... then open again
  }
  EXPECT_EQ(d.state(), std::optional<bool>{true});
  EXPECT_FALSE(d.sample(false, 1'000));
  EXPECT_TRUE(d.sample(false, 1'000 + TamperDebouncer::kDebounceMs));
  EXPECT_EQ(d.state(), std::optional<bool>{false});
}

TEST(Tamper, SameInputsSameResult) {
  // Determinism: the debounced output depends only on the samples and their times.
  const std::pair<TimeMs, bool> trace[] = {{0, false}, {60, false}, {70, true},  {90, false},
                                           {95, true}, {140, true}, {150, true}, {400, false}};
  std::optional<bool> first[8], second[8];
  for (auto* out : {first, second}) {
    TamperDebouncer d;
    for (int i = 0; i < 8; ++i) {
      d.sample(trace[i].second, trace[i].first);
      out[i] = d.state();
    }
  }
  for (int i = 0; i < 8; ++i) EXPECT_EQ(first[i], second[i]) << i;
  EXPECT_EQ(first[6], std::optional<bool>{true});
}

// --- service button ---------------------------------------------------------------------

TEST(ServiceButton, OnePressPerDebouncedPressAfterAKnownRelease) {
  ServiceButton b;
  EXPECT_FALSE(b.sample(false, 0));
  EXPECT_FALSE(b.sample(false, ServiceButton::kDebounceMs));  // released: baseline, not a press
  EXPECT_FALSE(b.sample(true, 100));
  EXPECT_FALSE(b.sample(true, 100 + ServiceButton::kDebounceMs - 1));
  EXPECT_TRUE(b.sample(true, 100 + ServiceButton::kDebounceMs));
  EXPECT_FALSE(b.sample(true, 500));  // held: still one press
  EXPECT_FALSE(b.sample(false, 600));
  EXPECT_FALSE(b.sample(false, 600 + ServiceButton::kDebounceMs));  // release is not a press
  EXPECT_FALSE(b.sample(true, 700));
  EXPECT_FALSE(b.sample(false, 710));  // 10 ms glitch
  EXPECT_FALSE(b.sample(false, 800));
}

TEST(ServiceButton, HeldThroughBootDoesNothing) {
  ServiceButton b;
  EXPECT_FALSE(b.sample(true, 0));
  EXPECT_FALSE(b.sample(true, ServiceButton::kDebounceMs));  // first known level: pressed
  EXPECT_FALSE(b.sample(true, 1'000));
}

}  // namespace
}  // namespace talos::fw
