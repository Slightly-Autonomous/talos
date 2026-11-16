// Board support for universal_controller_v1 rev proto-a.
//
// PROVISIONAL: every GPIO number below is a placeholder chosen to avoid the
// ESP32-H2 strapping, flash and USB pins as commonly documented. It is NOT a
// validated pin map (spec §31, §55). Replace from the real schematic and the
// board manifest before any hardware bring-up.
#pragma once

#include "firmware/portable/hal.h"

namespace talos::fw::board {

inline constexpr int kI2cSda = 10;                              // placeholder
inline constexpr int kI2cScl = 11;                              // placeholder
inline constexpr int kRail5vEnable[kSlots] = {-1, -1, 12, -1};  // only slot 2 has 5 V

inline BoardSupport proto_a() {
  BoardSupport b;
  const std::uint8_t wake[kSlots] = {0, 1, 4, 5};  // placeholders
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    b.slots[i] = SlotWiring{true, wake[i], i, kRail5vEnable[i] >= 0};
  }
  return b;
}

}  // namespace talos::fw::board
