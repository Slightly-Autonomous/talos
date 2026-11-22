// Board support for TL-SN-001 Rev A (sensor_node_proto_a).
//
// PROVISIONAL: GPIO numbers come from hardware/pins/esp32h2_mini1_pin_ledger.csv,
// sourced from the ESP32-H2-MINI-1 datasheet v1.6 (Table 3) and the ESP32-H2
// Series datasheet v1.3 §2.2 (LP pins GPIO8-14 work in every chip mode, so they
// are the only deep-sleep wake candidates). No board has been built or measured.
// Every ext1 wake line is now active high (slot WAKE, and EXP_WAKE through the
// SN74AUP1G04 inverter), so one ANY_HIGH mask covers them all and the H2's
// per-pin wake levels (HD-004) no longer matter for this board (HD-034).
// Keep this table, the ledger, the KiCad schematic and
// contracts/manifests/boards/sensor_node_proto_a.yaml in agreement.
#pragma once

#include "firmware/portable/hal.h"

namespace talos::fw::board {

inline constexpr int kI2cSda = 2;  // root I2C, MINI-1 pad 5
inline constexpr int kI2cScl = 3;  // root I2C, MINI-1 pad 6
// Rev A has no switched 5 V rail (HW-0003); contact 5 of sensor_edge_v1 is reserved.
inline constexpr int kRail5vEnable[kSlots] = {-1, -1, -1, -1};
inline constexpr std::uint8_t kSlotWake[kSlots] = {10, 11, 12, 13};  // LP GPIOs, pads 20/21/16/12
// EXP_WAKE: the three TCA9535 INT outputs, wired-OR on EXP_INT_N (active low,
// 47k pull-up), through an SN74AUP1G04 inverter: ACTIVE HIGH at the GPIO. LP GPIO.
inline constexpr std::uint8_t kExpanderWake = 14;
// Status LED D1: strap pin (must read high at reset for SPI boot), active low,
// 10k pull-up. Driven only after boot; idle high = off.
inline constexpr std::uint8_t kStatusLed = 8;
// BOOT button SW2 (BOOT_N, external pull-up): a strap at reset (held low there
// = download mode, handled by the ROM). After boot a press toggles slot-LED
// service mode. Input only, never driven.
inline constexpr std::uint8_t kBootButton = 9;
inline constexpr int kBatterySenseAdc = 1;  // ADC1_CH0, 4.7M/4.7M divider
// TCA9546A RESET_N through R_MUXRST (0R) from the U0RXD pad; R33 100k keeps it
// high until the GPIO is configured. U0RXD is also Tag-Connect J10 pin 6: once
// the app runs it drives this pin, so a host must not transmit on J10 RX (bench:
// TX-only boot log). In download mode the ROM owns it and a host TX only pulses
// the mux reset, which is harmless. Pull the 0R to use UART0 RX for a console.
inline constexpr std::uint8_t kMuxReset = 23;
// Shared SPI to all four slots (ledger pads 18/19/9): SCK = IO4 (MTCK, weak
// pull-up after reset), MOSI = IO5 (MTDI), MISO = IO0. Parked at boot by
// esp::Gpio. Digital pads: they lose this state in deep sleep, so the board has
// 100 kOhm pull-downs on SCK/MOSI/MISO (HD-027). Between reset and esp::Gpio,
// GPIO4's internal pull-up (~45 kOhm) fights SCK's 100k pull-down and wins:
// SCK idles near 2.3 V for those few ms. Harmless: on a cold boot every card is
// unpowered until discovery, long after esp::Gpio parked SCK low. After a
// deep-sleep wake the cards are powered, so esp::Gpio must stay the first thing
// app_main constructs (bench: scope SCK across a wake, PROVISIONAL).
inline constexpr std::uint8_t kSpiSck = 4;
inline constexpr std::uint8_t kSpiMosi = 5;
inline constexpr std::uint8_t kSpiMiso = 0;

// I/O expanders and charger on the root I2C bus (intended-netlist.json):
//   EXP_A 0x20: P00-P03 SLOT_SW_EN0-3, P04-P07 SLOT_AON_EN0-3,
//               P10-P13 SLOTn_RESET_N, P14-P17 SLOTn_CS_N
//   EXP_B 0x21: P00-P03 SLOTn_PRESENT_N, P04-P07 SLOTn_ALERT_N, P10/P11 UART_SEL0/1,
//               P12 UART_MUX_EN_N, P13 PRESENT_PROBE, P14 CHG_INT_N, P15 CHG_PG_N,
//               P16 PMUX_ST, P17 SW_FAULT_N (wired-OR, 100k pull-up)
//   EXP_C 0x22 (A0 = GND, A1 = V3V3_CORE, A2 = GND):
//               P00-P03 SLOT_LED0_N-SLOT_LED3_N (green, 2.2k to V3V3_CORE, active low),
//               P04 TAMPER_N (J9 lid loop to GND, 1M pull-up + 10 nF, tau 10 ms; high = open/cut),
//               P05-P07, P10-P17 not connected: outputs driven low.
//   BQ25186 0x6A. Pin n of port 1 is expander pin 8 + n.
inline constexpr std::uint8_t kExpA = 0x20;
inline constexpr std::uint8_t kExpB = 0x21;
inline constexpr std::uint8_t kExpC = 0x22;
inline constexpr std::uint8_t kCharger = 0x6A;

inline BoardSupport tl_sn_001() {
  BoardSupport b;
  b.name = "TL-SN-001";
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
  for (std::uint8_t i = 0; i < kSlots; ++i) b.slot_led_n[i] = {kExpC, i};
  b.tamper_n = {kExpC, 4};
  b.unused_pins[0] = {kExpC, 0xFFE0};  // P05-P07, P10-P17
  b.expander_wake_line = kExpanderWake;
  b.status_led_line = kStatusLed;
  b.service_button_line = kBootButton;
  b.mux_reset_line = kMuxReset;
  b.spi_sck_line = kSpiSck;
  b.spi_mosi_line = kSpiMosi;
  b.spi_miso_line = kSpiMiso;
  b.charger_address = kCharger;
  // Module admission budget on USB (provisional): every slot rail hangs off the TPS63900, which
  // guarantees 0.4 A x 3.3 V = 1.32 W; the MCU's 802.15.4 TX burst takes ~0.5 W of it. The generic
  // 2500 mW default would admit loads the regulator cannot supply (hardware/results/pi.md, S-2).
  b.usb_sustained_mw = 800;
  // Archived modular prototype (ADR 0019): its USB-C sits inside the enclosure, so
  // the console stays fully open and the lamps keep the owner's "lit on USB" rule.
  // TL-SN-002 locks both (board_tl_sn_002.h).
  b.console_lock = ConsoleLock::kNever;
  b.lamp_gate = LampGate::kExternalPower;
  return b;
}

}  // namespace talos::fw::board
