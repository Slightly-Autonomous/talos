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
// TCA9546A RESET_N through R_MUXRST (0R) from the U0RXD pad; R33 100k keeps it
// high until the GPIO is configured. Pull the 0R to use UART0 RX for a console.
inline constexpr std::uint8_t kMuxReset = 23;

// I/O expanders and charger on the root I2C bus (intended-netlist.json):
//   EXP_A 0x20: P00-P03 SLOT_SW_EN0-3, P04-P07 SLOT_AON_EN0-3,
//               P10-P13 SLOTn_RESET_N, P14-P17 SLOTn_CS_N
//   EXP_B 0x21: P00-P03 SLOTn_PRESENT_N, P04-P07 SLOTn_ALERT_N, P10/P11 UART_SEL0/1,
//               P12 UART_MUX_EN_N, P13 PRESENT_PROBE, P14 CHG_INT_N, P15 CHG_PG_N,
//               P16 PMUX_ST, P17 SW_FAULT_N (wired-OR, 100k pull-up)
//   BQ25186 0x6A. Pin n of port 1 is expander pin 8 + n.
inline constexpr std::uint8_t kExpA = 0x20;
inline constexpr std::uint8_t kExpB = 0x21;
inline constexpr std::uint8_t kCharger = 0x6A;

inline BoardSupport tl_sn_001() {
  BoardSupport b;
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    b.slots[i] = SlotWiring{true, kSlotWake[i], i, kRail5vEnable[i] >= 0};
    auto& io = b.slot_io[i];
    io.sw_enable = {kExpA, i};
    io.aon_enable = {kExpA, static_cast<std::uint8_t>(4 + i)};
    io.reset_n = {kExpA, static_cast<std::uint8_t>(8 + i)};
    io.cs_n = {kExpA, static_cast<std::uint8_t>(12 + i)};
    io.present_n = {kExpB, i};
    io.alert_n = {kExpB, static_cast<std::uint8_t>(4 + i)};
  }
  b.uart_sel0 = {kExpB, 8};
  b.uart_sel1 = {kExpB, 9};
  b.uart_mux_en_n = {kExpB, 10};
  b.present_probe = {kExpB, 11};
  b.charger_int_n = {kExpB, 12};
  b.charger_pg_n = {kExpB, 13};
  b.source_st = {kExpB, 14};
  b.sw_fault_n = {kExpB, 15};
  b.mux_reset_line = kMuxReset;
  b.charger_address = kCharger;
  return b;
}

}  // namespace talos::fw::board
