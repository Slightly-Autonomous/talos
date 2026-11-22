// Board support for TL-SN-002 Rev A, the self-contained sensor node (ADR 0019).
//
// PROVISIONAL: every number here waits for the TL-SN-002 schematic review and
// the bench. GPIO capabilities follow the ESP32-H2-MINI-1 datasheet v1.6 and the
// ESP32-H2 Series datasheet v1.3 §2.2: only LP GPIO8-14 can wake the chip from
// deep sleep, so the five wake sources sit on GPIO10-14, all ACTIVE HIGH, and
// one ext1 ANY_HIGH mask covers them (the TMAG5273 INT_N keeps its SN74AUP1G04
// inverter, owner decision 2026-11-21, so HD-004's per-pin polarity is not needed).
// Keep this table and contracts/manifests/boards/sensor_node_v2.yaml in
// agreement (tools/dbg/tests/test_contracts.py checks the two).
//
// Built-in sensors are fixed "slots" (SlotWiring::fixed_module): no mux, no ID
// EEPROMs, no presence lines, no per-slot rails except PIR_PWR.
//   slot 0  MAG  TMAG5273A2  0x35  INT_N -> SN74AUP1G04 -> GPIO10 (MAG_WAKE)
//   slot 1  VIB  LIS2DW12    0x19  INT1 push-pull active high -> GPIO11 (SA0 high, CS high)
//   slot 2  PIR  PIR223D-S0  -     digital out, 1M pull-down -> GPIO12; rail = EXP P10 PIR_PWR
//   slot 3  ENV  SHT40-AD1B  0x44  no interrupt
// The TMAG5273A2's factory address: the letter is the address variant (A = 0x35)
// and the digit the range (2 = +/-133/266 mT). Check against the datasheet's
// device comparison table before the schematic is frozen.
#pragma once

#include "firmware/portable/hal.h"
#include "firmware/portable/modules.h"

namespace talos::fw::board::sn002 {

inline constexpr const char* kBoardName = "TL-SN-002";
inline constexpr int kI2cSda = 2;  // root I2C, 4.7k pull-ups to 3V3
inline constexpr int kI2cScl = 3;
inline constexpr int kBatterySenseAdc = 1;  // ADC1_CH0: VBAT after reverse protection, 4.7M/4.7M
// VBUS through 100k/100k: high = USB powers the node (TPS2116 prefers IN1 = VBUS).
// GPIO4 is a digital pad (not LP): read while awake only, never a wake source.
inline constexpr std::uint8_t kUsbPresent = 4;
// Status LED (STAT): strap pin, active low, 10k pull-up. Driven only after boot.
inline constexpr std::uint8_t kStatusLed = 8;
// BOOT button: strap at reset (download mode, ROM), service-mode toggle after boot.
inline constexpr std::uint8_t kBootButton = 9;
// Five LP wake lines, all active high (ext1 ANY_HIGH).
inline constexpr std::uint8_t kMagWake = 10;  // TMAG5273A2 INT_N through SN74AUP1G04
inline constexpr std::uint8_t kAccInt = 11;   // LIS2DW12 INT1, push-pull active high
inline constexpr std::uint8_t kPirOut = 12;   // PIR223D-S0 output, 1M pull-down
inline constexpr std::uint8_t kTamper =
    13;  // SW4 lid + SW5 battery door to GND, 1M pull-up + 10 nF
inline constexpr std::uint8_t kUserButton = 14;  // to 3V3, 100k pull-down
// GPIO0, 5, 22, 25: spare (GPIO0 on a test pad). GPIO23/24: U0RXD/U0TXD on the
// Tag-Connect recovery pads. GPIO26/27: USB Serial/JTAG (flash, console, debug).

// TCA9535 at 0x20 (A0-A2 GND). INT not connected: its one input (P11) is read
// when the tamper line (GPIO13, a wake source) says the loop opened.
//   P00 LED_LINK_N  P01 LED_MAG_N  P02 LED_VIB_N  P03 LED_PIR_N
//   P04 LED_ENV_N   P05 LED_TAMP_N P06 LED_BATT_N P07 LED_FLT_N   (active low, 2.2k to 3V3)
//   P10 PIR_PWR (high = PIR powered through an RC filter; the expander holds it in deep sleep)
//   P11 TAMPER_DOOR in (4.7M pull-up, SW5 to GND: high = battery door open, HD-049)
//   P12-P17 unused: outputs driven low.
inline constexpr std::uint8_t kExp = 0x20;
inline constexpr std::uint8_t kPirPowerPin = 8;     // P10
inline constexpr std::uint8_t kBatteryDoorPin = 9;  // P11

inline BoardSupport tl_sn_002() {
  BoardSupport b;
  b.name = kBoardName;
  b.mux_address = 0;  // no mux: the built-in sensors share the root bus
  auto fixed = [](std::uint8_t wake, ModuleCode code) {
    SlotWiring w;
    w.present = true;
    w.wake_line = wake;
    w.fixed_module = static_cast<std::uint16_t>(code);
    return w;
  };
  b.slots[0] = fixed(kMagWake, ModuleCode::kTmag5273A2);
  b.slots[1] = fixed(kAccInt, ModuleCode::kLis2dw12);
  b.slots[2] = fixed(kPirOut, ModuleCode::kPir223d);
  b.slots[3] = fixed(0xFF, ModuleCode::kSht40);
  b.slot_io[2].aon_enable = {kExp, kPirPowerPin};  // PIR_PWR is the PIR "slot"'s only rail
  // Per-sensor activity lamps reuse the slot-LED policy (L-03).
  b.slot_led_n[0] = {kExp, 1};  // MAG
  b.slot_led_n[1] = {kExp, 2};  // VIB
  b.slot_led_n[2] = {kExp, 3};  // PIR
  b.slot_led_n[3] = {kExp, 4};  // ENV
  b.lamp_n[static_cast<std::size_t>(Lamp::kLink)] = {kExp, 0};
  b.lamp_n[static_cast<std::size_t>(Lamp::kTamper)] = {kExp, 5};
  b.lamp_n[static_cast<std::size_t>(Lamp::kBattery)] = {kExp, 6};
  b.lamp_n[static_cast<std::size_t>(Lamp::kFault)] = {kExp, 7};
  b.unused_pins[0] = {kExp, 0xFC00};  // P12-P17
  b.battery_door = {kExp, kBatteryDoorPin};
  b.status_led_line = kStatusLed;
  b.service_button_line = kBootButton;
  b.tamper_line = kTamper;
  b.user_button_line = kUserButton;
  b.usb_present_line = kUsbPresent;
  // 2 x AA Li-FeS2 (Energizer L91 class), primary: ~3.6 V fresh (open circuit),
  // ~3.0 V plateau under load, then a knee. Thresholds are pair voltages UNDER
  // LOAD (after a radio burst). PROVISIONAL. Low = 2.2 V (1.10 V/cell) from
  // hardware/results/power-budget-sn2.md: it keeps the TPS63900 input >= 1.8 V
  // during an 18 dBm TX burst at -40 C. Below it the node reports low battery
  // (BATT lamp, PowerMode::kLowPower) and caps TX power at +10 dBm (about half
  // the burst current of +18 dBm, ~11 dB less link margin). PROVISIONAL (HD-043).
  b.primary_battery = true;
  b.battery_low_mv = 2200;
  b.battery_critical_mv = 2000;  // 1.0 V/cell loaded: the 1.8 V regulator floor is close
  b.battery_ota_min_mv = 2600;   // an OTA is minutes of radio: only on healthy cells
  // The loaded reading is noisy (burst length, temperature): leaving low power or
  // critical needs 100 mV above the threshold, so the mode can't flap per heartbeat.
  b.battery_hysteresis_mv = 100;
  b.tx_power_dbm = 18;
  b.tx_power_low_battery_dbm = 10;
  // Owner 2026-11-22: deep sleep between reports by default (<= 2 s event
  // latency); the hub can switch a node to kSleepyChild or kAlwaysOn per device.
  b.power_profile = PowerProfile::kDeepSleep;
  // Module admission budgets (provisional). Battery: card-equivalent draw only,
  // the board baseline is in the power budget. USB: TPS63900 0.4 A x 3.3 V minus
  // the radio TX burst, as TL-SN-001.
  b.battery_sustained_mw = 15;
  b.usb_sustained_mw = 800;
  // The USB-C port stays reachable from outside the closed enclosure (owner,
  // 2026-11-22): the console is read-only while the lid is closed, and the
  // firmware lamps (STAT included) light only with the lid open or in service
  // mode, so a cable or power bank on the outside port changes and reveals nothing.
  // The hardware PWR lamp on VBUS stays: it only says USB power is present.
  b.console_lock = ConsoleLock::kLidClosed;
  b.lamp_gate = LampGate::kLidOpen;
  return b;
}

}  // namespace talos::fw::board::sn002
