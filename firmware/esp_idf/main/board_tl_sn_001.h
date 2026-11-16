// Board support for TL-SN-001 Rev A (sensor_node_proto_a).
//
// PROVISIONAL: GPIO numbers come from hardware/pins/esp32h2_mini1_pin_ledger.csv,
// sourced from the ESP32-H2-MINI-1 datasheet v1.6 (Table 3) and the ESP32-H2
// Series datasheet v1.3 §2.2 (LP pins GPIO8-14 work in every chip mode, so they
// are the only deep-sleep wake candidates). No board has been built or measured;
// per-pin ext1 wake levels on the H2 are unverified (HD-004). Keep this table,
// the ledger and the KiCad schematic in agreement.
#pragma once

#include "firmware/portable/hal.h"

namespace talos::fw::board {

inline constexpr int kI2cSda = 2;  // root I2C, MINI-1 pad 5
inline constexpr int kI2cScl = 3;  // root I2C, MINI-1 pad 6
// Rev A has no switched 5 V rail (HW-0003); contact 5 of sensor_edge_v1 is reserved.
inline constexpr int kRail5vEnable[kSlots] = {-1, -1, -1, -1};
inline constexpr std::uint8_t kSlotWake[kSlots] = {10, 11, 12, 13};  // LP GPIOs, pads 20/21/16/12
inline constexpr int kExpanderInt = 14;     // TCA9535 INT wired-OR, LP GPIO
inline constexpr int kStatusLed = 8;        // strap pin, active low, 10k pull-up
inline constexpr int kBatterySenseAdc = 1;  // ADC1_CH0, 4.7M/4.7M divider

inline BoardSupport tl_sn_001() {
  BoardSupport b;
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    b.slots[i] = SlotWiring{true, kSlotWake[i], i, kRail5vEnable[i] >= 0};
  }
  return b;
}

}  // namespace talos::fw::board
