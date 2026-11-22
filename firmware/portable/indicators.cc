#include "firmware/portable/indicators.h"

namespace talos::fw {

namespace {
constexpr TimeMs kNone = ~TimeMs{0};
}  // namespace

// --- Status LED -------------------------------------------------------------------------

StatusCode select_status(const StatusInputs& in) {
  if (in.fault) return StatusCode::kFault;
  if (in.cold_boot && in.since_boot_ms < kBootShowMs) return StatusCode::kBoot;
  if (in.card_error) return StatusCode::kCardError;
  if (in.ota_active) return StatusCode::kOta;
  if (in.low_battery) return StatusCode::kLowBattery;
  if (!in.hub_heard || in.link_failing) return StatusCode::kJoining;
  if (in.external_power) return StatusCode::kCharging;
  return StatusCode::kJoined;
}

bool status_led_on(StatusCode c, TimeMs phase_ms) {
  const BlinkCode b = blink_code(c);
  if (b.pulses == 0 || b.period_ms == 0) return false;
  const TimeMs t = phase_ms % b.period_ms;
  const TimeMs step = kStatusPulseMs + b.gap_ms;
  const TimeMs k = t / step;
  return k < b.pulses && t - k * step < kStatusPulseMs;
}

TimeMs status_next_edge(StatusCode c, TimeMs phase_ms) {
  const BlinkCode b = blink_code(c);
  if (b.pulses == 0 || b.period_ms == 0) return kNone;
  const TimeMs base = phase_ms - phase_ms % b.period_ms;
  const TimeMs t = phase_ms % b.period_ms;
  const TimeMs step = kStatusPulseMs + b.gap_ms;
  // Edges inside one period: each pulse's rise and fall; then the next period.
  for (std::uint8_t k = 0; k < b.pulses; ++k) {
    const TimeMs rise = k * step;
    const TimeMs fall = rise + kStatusPulseMs;
    if (rise > t) return base + rise;
    if (fall > t) return base + fall;
  }
  return base + b.period_ms;
}

// --- Slot LEDs --------------------------------------------------------------------------

void SlotLedPolicy::note_wake(std::uint8_t slot, TimeMs now) {
  if (slot < kSlots) flash_until_[slot] = now + kFlashMs;
}

bool SlotLedPolicy::led_on(std::uint8_t slot, SlotLedMode mode, TimeMs now,
                           bool external_power) const {
  if (slot >= kSlots || !lit_allowed(now, external_power)) return false;
  bool on = false;
  switch (mode) {
    case SlotLedMode::kOff:
      on = false;
      break;
    case SlotLedMode::kSolid:
      on = true;
      break;
    case SlotLedMode::kSlowBlink:
      on = now % kSlowBlinkPeriodMs < kSlowBlinkOnMs;
      break;
  }
  // A short flash is the base state inverted: visible on a dark and a lit LED.
  if (now < flash_until_[slot]) on = !on;
  return on;
}

TimeMs SlotLedPolicy::next_change(const std::array<SlotLedMode, kSlots>& modes, TimeMs now,
                                  bool external_power) const {
  TimeMs next = kNone;
  auto consider = [&](TimeMs t) {
    if (t > now && t < next) next = t;
  };
  // Service mode ending turns everything dark (unless on external power).
  if (!external_power && service_active(now)) consider(service_until_);
  if (!lit_allowed(now, external_power)) return next;
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    consider(flash_until_[i]);
    if (modes[i] == SlotLedMode::kSlowBlink) {
      const TimeMs t = now % kSlowBlinkPeriodMs;
      consider(now - t + (t < kSlowBlinkOnMs ? kSlowBlinkOnMs : kSlowBlinkPeriodMs));
    }
  }
  return next;
}

// --- Tamper -----------------------------------------------------------------------------

bool LevelDebouncer::sample(std::optional<bool> raw_open, TimeMs now) {
  if (!raw_open) return false;  // a failed read proves nothing either way
  const bool v = *raw_open;
  if (state_ && *state_ == v) {
    pending_ = false;  // a bounce that settled back
    return false;
  }
  if (!pending_ || candidate_ != v) {
    pending_ = true;
    candidate_ = v;
    since_ = now;
    return false;
  }
  if (now < since_ + debounce_ms_) return false;
  state_ = v;
  pending_ = false;
  return true;
}

bool TamperDebouncer::sample(std::optional<bool> raw_open, TimeMs now) {
  if (!raw_open) return false;  // a failed read proves nothing either way
  if (*raw_open) {
    closed_run_ = false;
    if (state_ == true) return false;
    if (!opening_) {
      opening_ = true;
      open_since_ = now;
    }
    if (now < open_since_ + kDebounceMs) return false;
    state_ = true;
    opening_ = false;
    return true;
  }
  if (state_ == false && !opening_) return false;
  if (!closed_run_) {
    closed_run_ = true;
    closed_since_ = now;
  }
  if (now < closed_since_ + kDebounceMs) return false;
  closed_run_ = false;
  opening_ = false;  // the closed level held: the opening was a bounce
  if (state_ == false) return false;
  state_ = false;
  return true;
}

// --- USER button ------------------------------------------------------------------------

UserButton::Press UserButton::sample(bool pressed, TimeMs now) {
  const bool was_known = level_.state().has_value();
  if (!level_.sample(pressed, now)) {
    // Long press: reported while the button is still down.
    if (held_ && armed_ && !long_sent_ && now >= pressed_at_ + kLongPressMs) {
      long_sent_ = true;
      return Press::kLong;
    }
    return Press::kNone;
  }
  const bool down = *level_.state();
  if (down) {
    held_ = true;
    long_sent_ = false;
    pressed_at_ = now - kDebounceMs;  // the press began when the level first changed
    if (!was_known) armed_ = false;   // held through a cold boot: ignore this press
    return Press::kNone;
  }
  const bool counted = held_ && armed_ && !long_sent_;
  held_ = false;
  armed_ = true;
  return counted ? Press::kShort : Press::kNone;
}

void UserButton::begin_held(TimeMs now) {
  level_.restore(true);
  held_ = true;
  armed_ = true;
  long_sent_ = false;
  pressed_at_ = now;
}

TimeMs UserButton::due_ms() const {
  if (level_.pending()) return level_.due_ms();
  if (held_ && armed_ && !long_sent_) return pressed_at_ + kLongPressMs;
  return kNone;
}

// --- Board lamps ------------------------------------------------------------------------

std::uint8_t lamp_levels(const LampInputs& in, TimeMs now) {
  if (!in.lit_allowed) return 0;
  std::uint8_t out = 0;
  auto set = [&](Lamp l, bool on) {
    if (on) out = static_cast<std::uint8_t>(out | (1u << static_cast<unsigned>(l)));
  };
  // Joining or failing: the slot LEDs' slow blink (1 s on, 1 s off).
  set(Lamp::kLink,
      in.joined || now % SlotLedPolicy::kSlowBlinkPeriodMs < SlotLedPolicy::kSlowBlinkOnMs);
  set(Lamp::kTamper, in.tamper_open.value_or(false));
  set(Lamp::kBattery, in.battery_low);
  set(Lamp::kFault, in.fault);
  return out;
}

}  // namespace talos::fw
