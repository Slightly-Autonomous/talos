// Indicators and the tamper loop on TL-SN-001 (owner decisions L-01, L-03,
// L-04, HD-032). Small pure state machines: no I/O, no heap, time only from
// the values the runtime passes in (taken from the injected IClock), so the
// same inputs always give the same LED levels and the same events.
//
// - Status LED D1 (GPIO8, active low): one blink code at a time, chosen by
//   priority. Every pulse is kStatusPulseMs (20 ms) on, so the steady-state
//   duty is at most 2 % (joining) and 0.2 % once joined.
// - Slot LEDs S0..S3 (EXP_C P00-P03, active low): dark on battery. An LED that
//   blinks when someone walks past tells him he was seen, and the battery has
//   no budget for it. Lit only on external power (USB or PV, from CHG_PG_N and
//   PMUX_ST) or for kServiceMs after a service request.
// - Tamper (TAMPER_N, EXP_C P04): debounced; high = lid open or loop cut.
//
// PROVISIONAL: timings are first guesses, not checked on a board.
#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "firmware/portable/types.h"

namespace talos::fw {

// --- Status LED (L-01, L-04) ------------------------------------------------------------

// In ascending priority except kOff. select_status() picks one.
enum class StatusCode : std::uint8_t {
  kOff = 0,
  kJoined = 1,      // the hub has answered since boot
  kCharging = 2,    // joined, and external power is present (L-04)
  kJoining = 3,     // no answer from the hub yet, or the link is failing
  kLowBattery = 4,  // PowerMode low-power or critical
  kOta = 5,         // firmware download or trial image awaiting confirmation
  kCardError = 6,   // a slot has a fault (not "disabled for power")
  kBoot = 7,        // the first kBootShowMs after a cold boot
  kFault = 8,       // board did not take its safe state, or the epoch is not persisted
};

inline constexpr std::uint32_t kStatusPulseMs = 20;
inline constexpr std::uint32_t kBootShowMs = 3'000;

// `pulses` pulses of kStatusPulseMs, each followed by `gap_ms` off, repeated
// every `period_ms` (measured from the start of the first pulse).
struct BlinkCode {
  std::uint8_t pulses = 0;
  std::uint16_t gap_ms = 0;
  std::uint32_t period_ms = 0;
};

// The blink-code table (docs/hardware/bench-test-plan.md "Bring-up reference").
constexpr BlinkCode blink_code(StatusCode c) {
  switch (c) {
    case StatusCode::kOff:
      return {0, 0, 1'000};
    case StatusCode::kJoined:
      return {1, 0, 10'000};
    case StatusCode::kCharging:
      return {2, 300, 4'000};
    case StatusCode::kJoining:
      return {1, 0, 1'000};
    case StatusCode::kLowBattery:
      return {2, 300, 15'000};
    case StatusCode::kOta:
      return {3, 150, 2'000};
    case StatusCode::kCardError:
      return {3, 300, 5'000};
    case StatusCode::kBoot:
      return {1, 0, 250};
    case StatusCode::kFault:
      return {4, 300, 5'000};
  }
  return {0, 0, 1'000};
}

struct StatusInputs {
  TimeMs since_boot_ms = 0;
  bool cold_boot = true;  // a deep-sleep wake does not replay the boot code
  bool hub_heard = false;
  bool link_failing = false;
  bool ota_active = false;
  bool low_battery = false;
  bool card_error = false;
  bool fault = false;
  bool external_power = false;
};

StatusCode select_status(const StatusInputs& in);
// LED level `phase_ms` after the code started.
bool status_led_on(StatusCode c, TimeMs phase_ms);
// The next phase, strictly after `phase_ms`, at which the level can change.
TimeMs status_next_edge(StatusCode c, TimeMs phase_ms);

// --- Slot LEDs (L-03) -------------------------------------------------------------------

enum class SlotLedMode : std::uint8_t {
  kOff = 0,
  kSolid = 1,      // card enumerated and running
  kSlowBlink = 2,  // fault or quarantine
};

class SlotLedPolicy {
 public:
  static constexpr std::uint32_t kServiceMs = 10u * 60u * 1000u;  // service mode timeout
  static constexpr std::uint32_t kFlashMs = 150;  // a wake event inverts the LED this long
  static constexpr std::uint32_t kSlowBlinkPeriodMs = 2'000;
  static constexpr std::uint32_t kSlowBlinkOnMs = 1'000;

  void start_service(TimeMs now) { service_until_ = now + kServiceMs; }
  void stop_service() { service_until_ = 0; }
  bool service_active(TimeMs now) const { return now < service_until_; }
  TimeMs service_until() const { return service_until_; }
  // A wake event from this slot (motion, contact, impact): a short flash.
  void note_wake(std::uint8_t slot, TimeMs now);

  bool lit_allowed(TimeMs now, bool external_power) const {
    return external_power || service_active(now);
  }
  bool led_on(std::uint8_t slot, SlotLedMode mode, TimeMs now, bool external_power) const;
  // Earliest time after `now` at which any LED can change (~0, i.e. kNever, if none).
  TimeMs next_change(const std::array<SlotLedMode, kSlots>& modes, TimeMs now,
                     bool external_power) const;

 private:
  TimeMs service_until_ = 0;
  std::array<TimeMs, kSlots> flash_until_{};
};

// --- Tamper loop (HD-032) ---------------------------------------------------------------

// A level must hold kDebounceMs before it counts. Readings that failed
// (nullopt) neither confirm nor reset a pending change.
class LevelDebouncer {
 public:
  explicit LevelDebouncer(std::uint32_t debounce_ms) : debounce_ms_(debounce_ms) {}

  // Returns true when the debounced state changed (including the first one).
  bool sample(std::optional<bool> raw_open, TimeMs now);
  // Debounced state: nullopt until a level has held the debounce time once.
  std::optional<bool> state() const { return state_; }
  bool pending() const { return pending_; }
  TimeMs due_ms() const { return since_ + debounce_ms_; }
  // Deep-sleep wake: the state from before the sleep, so the reading that woke
  // the node shows up as a change.
  void restore(bool open) {
    state_ = open;
    pending_ = false;
  }

 private:
  std::uint32_t debounce_ms_;
  std::optional<bool> state_;
  bool pending_ = false;
  bool candidate_ = false;
  TimeMs since_ = 0;
};

// The tamper loop: 50 ms (TL-SN-001 RC 1 MOhm / 10 nF reaches V_IH in ~12 ms).
// Asymmetric, failing toward "open": once the loop opens, only a closed level
// that holds kDebounceMs cancels the opening. Short closures do not restart its
// timer, so a loop chattered open/closed faster than the debounce (a reed
// switch pulsed by a magnet, a wire tapped against GND) is still reported
// open kDebounceMs after it first opened, at the next open reading. A single
// knock (open < kDebounceMs, then closed for kDebounceMs) is still ignored.
// Restoring (open to closed) is symmetric: closed must hold, any open reading
// restarts it, so chatter delays a restore and never hides an opening.
class TamperDebouncer {
 public:
  static constexpr std::uint32_t kDebounceMs = 50;

  // Returns true when the debounced state changed (including the first one).
  bool sample(std::optional<bool> raw_open, TimeMs now);
  std::optional<bool> state() const { return state_; }
  bool pending() const { return opening_ || closed_run_; }
  // When the next timed decision falls (a reading is due then).
  TimeMs due_ms() const {
    return closed_run_ ? closed_since_ + kDebounceMs : open_since_ + kDebounceMs;
  }
  void restore(bool open) {
    state_ = open;
    opening_ = false;
    closed_run_ = false;
  }

 private:
  std::optional<bool> state_;
  bool opening_ = false;  // an opening is pending (state closed or unknown)
  TimeMs open_since_ = 0;
  bool closed_run_ = false;  // the line has read closed since closed_since_
  TimeMs closed_since_ = 0;
};

// --- Service button (BOOT, GPIO9) ------------------------------------------------------

// A press of the BOOT button toggles slot-LED service mode (no laptop needed).
// The line is read only after boot (it is a strap pin at reset); state true =
// pressed. A press counts once it has held kDebounceMs, and only after the
// button was seen released, so a button held through boot does nothing.
class ServiceButton {
 public:
  static constexpr std::uint32_t kDebounceMs = 30;
  // True once per debounced press.
  bool sample(bool pressed, TimeMs now) {
    const bool known = level_.state().has_value();
    return level_.sample(pressed, now) && known && *level_.state();
  }
  bool pending() const { return level_.pending(); }
  TimeMs due_ms() const { return level_.due_ms(); }

 private:
  LevelDebouncer level_{kDebounceMs};
};

// --- USER button (TL-SN-002, ADR 0019) ---------------------------------------------------

// The USER button (to 3V3, pull-down: high = pressed). Debounced like BOOT. A
// press released before kLongPressMs is a short press (reported on release); a
// press held kLongPressMs is a long press, reported once while still held, and
// its release reports nothing more. A button already held at a cold boot does
// nothing until it was seen released; after a deep-sleep wake from this line the
// press that woke the node counts from boot (begin_held()).
class UserButton {
 public:
  enum class Press : std::uint8_t { kNone, kShort, kLong };
  static constexpr std::uint32_t kDebounceMs = 30;
  static constexpr std::uint32_t kLongPressMs = 3'000;

  Press sample(bool pressed, TimeMs now);
  // The press that woke the node from deep sleep started at `now`.
  void begin_held(TimeMs now);
  bool held() const { return held_; }
  // Next time sample() must run without an edge: debounce end, or the long-press mark.
  TimeMs due_ms() const;
  bool pending() const { return level_.pending() || (held_ && !long_sent_ && armed_); }

 private:
  LevelDebouncer level_{kDebounceMs};
  bool armed_ = false;  // seen released at least once (or woken by it)
  bool held_ = false;
  bool long_sent_ = false;
  TimeMs pressed_at_ = 0;
};

// --- Board lamps (TL-SN-002) ------------------------------------------------------------

// Lamp test after a short USER press: every lamp blinks kLampTestHalfMs on/off
// for kLampTestMs, lit even on battery (the owner asked for it), then the normal
// policy (dark on battery unless service mode) takes over again.
inline constexpr std::uint32_t kLampTestMs = 10'000;
inline constexpr std::uint32_t kLampTestHalfMs = 250;
// FLT stays lit this long after the last fault report of any kind.
inline constexpr std::uint32_t kFaultLampMs = 10u * 60u * 1000u;

struct LampInputs {
  bool lit_allowed = false;  // external power or service mode
  bool joined = false;       // hub answering, link not failing
  std::optional<bool> tamper_open;
  bool battery_low = false;
  bool fault = false;
};

// Level of each Lamp at `now` (bit n = Lamp n), without the lamp test.
std::uint8_t lamp_levels(const LampInputs& in, TimeMs now);
// Lamp-test level at `now` for a test that started at `started`.
inline bool lamp_test_on(TimeMs started, TimeMs now) {
  return ((now - started) / kLampTestHalfMs) % 2 == 0;
}

}  // namespace talos::fw
