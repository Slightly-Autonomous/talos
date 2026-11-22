// How the node sleeps between steps (owner 2026-11-22). Pure: the platform asks
// NodeRuntime::sleep_plan() after each step and carries the plan out
// (ESP32-H2: esp_light_sleep_start / esp_deep_sleep_start with ext1 + timer).
//
//   kAlwaysOn     never sleeps; the radio receives continuously (USB power, or
//                 the owner chose instant reporting).
//   kSleepyChild  light sleep: RAM, radio attachment and timers survive; wake
//                 lines resume it within ~1 ms; the stack polls its parent.
//   kDeepSleep    deep sleep when nothing is due soon: a CPU reset on wake,
//                 then a Thread re-attach before the first send. Lowest current;
//                 event-to-hub latency is the rejoin (budget kDeepSleepLatencyMs).
//
// Deep sleep is not worth it for short gaps (boot + rejoin costs more charge
// than a short light sleep), and never while sends wait or the link is
// mid-exchange; those cases fall back to light sleep.
#pragma once

#include <cstdint>

#include "firmware/portable/types.h"

namespace talos::fw {

struct SleepPlan {
  enum class Kind : std::uint8_t { kStayAwake = 0, kLight = 1, kDeep = 2 };
  Kind kind = Kind::kStayAwake;
  TimeMs until_ms = 0;  // absolute wake time (timer); wake lines may end it sooner
  bool operator==(const SleepPlan&) const = default;
};

struct SleepInputs {
  PowerProfile profile = PowerProfile::kDeepSleep;
  bool external_power = false;  // USB: always on
  bool sends_pending = false;   // queued events not yet acknowledged
  bool console_active = false;  // a USB host talked to the console recently
  bool busy = false;            // OTA download, debounce, lamp test: keep stepping
  TimeMs now_ms = 0;
  TimeMs next_wake_ms = 0;  // NodeRuntime::next_wake_ms()
};

class SleepPolicy {
 public:
  // Gaps shorter than this are not slept through at all (the loop just waits).
  static constexpr TimeMs kMinLightMs = 5;
  // Deep sleep only for gaps at least this long: a wake costs a boot (~30 ms)
  // plus a Thread re-attach (~200-600 ms at ~10 mA), worth ~2 s of light sleep.
  static constexpr TimeMs kMinDeepMs = 5'000;
  // Design budget for event-to-hub latency out of deep sleep (owner: <= 2 s).
  static constexpr TimeMs kDeepSleepLatencyMs = 2'000;

  static SleepPlan plan(const SleepInputs& in) {
    const TimeMs gap = in.next_wake_ms > in.now_ms ? in.next_wake_ms - in.now_ms : 0;
    SleepPlan p;
    p.until_ms = in.next_wake_ms;
    if (in.external_power || in.profile == PowerProfile::kAlwaysOn || in.busy ||
        in.console_active || gap < kMinLightMs) {
      p.kind = SleepPlan::Kind::kStayAwake;
      return p;
    }
    if (in.profile == PowerProfile::kSleepyChild || in.sends_pending || gap < kMinDeepMs) {
      p.kind = SleepPlan::Kind::kLight;
      return p;
    }
    p.kind = SleepPlan::Kind::kDeep;
    return p;
  }
};

}  // namespace talos::fw
