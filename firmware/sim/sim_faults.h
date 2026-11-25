// Named faults for the virtual TL-SN-001 and TL-SN-002, shared by bench_node and tests so the
// bench fault trees (tools/bench) can be exercised against every one of them.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "firmware/sim/virtual_board.h"

namespace talos::fw::sim {

inline bool parse_card(const std::string& name, ModuleCode& code) {
  struct {
    const char* name;
    ModuleCode code;
  } const cards[] = {
      {"TL-MOD-001", ModuleCode::kTmag5273Contact}, {"TL-MOD-002", ModuleCode::kTlMod002Pir},
      {"TL-MOD-003", ModuleCode::kTlMod003Imu},     {"TL-MOD-004", ModuleCode::kTlMod004Env},
      {"TL-TEST-001", ModuleCode::kTlTest001},
  };
  for (const auto& c : cards) {
    if (name == c.name) {
      code = c.code;
      return true;
    }
  }
  return false;
}

// Applies one fault by name; false if the name is unknown. Slot-numbered faults
// end in the slot digit, e.g. "branch_open2", "blank_id0".
inline bool apply_fault(Node& n, const std::string& fault) {
  auto slot_of = [&](const char* prefix, std::uint8_t& slot) {
    const std::size_t len = std::strlen(prefix);
    if (fault.size() != len + 1 || fault.compare(0, len, prefix) != 0) return false;
    const char d = fault[len];
    if (d < '0' || d > '3') return false;
    slot = static_cast<std::uint8_t>(d - '0');
    return true;
  };
  std::uint8_t s = 0;
  if (n.is_sn002()) {
    // TL-SN-002 (ADR 0019): built-in sensors, one expander, direct lines.
    Sn002Board& b = n.sn002();
    if (fault == "lid_open") {
      b.lid_open = true;
    } else if (fault == "lid_closed") {
      b.lid_open = false;
    } else if (fault == "usb_off") {
      b.usb = false;  // unplugged: the power mux falls back to the cells
    } else if (fault == "usb_on") {
      b.usb = true;
    } else if (fault == "user_press") {
      b.user_pressed = true;
    } else if (fault == "user_release") {
      b.user_pressed = false;
    } else if (fault == "boot_press") {
      n.gpio.set(n.board.service_button_line, false);  // BOOT_N low (after boot: service mode)
    } else if (fault == "boot_release") {
      n.gpio.set(n.board.service_button_line, true);
    } else if (fault == "exp_nack") {
      b.exp.nack = true;
    } else if (fault == "pir_stuck") {
      b.pir_stuck_high = true;
    } else if (fault == "tmag_missing") {
      n.i2c.detach(I2cBus::kRoot, Tmag5273ContactDriver::kAddr);    // A1 (archived card)
      n.i2c.detach(I2cBus::kRoot, Tmag5273ContactDriver::kX2Addr);  // TL-SN-002's B2
    } else if (fault == "lis_missing") {
      n.i2c.detach(I2cBus::kRoot, Lis2dw12Driver::kAddr);
    } else if (fault == "sht_missing") {
      n.i2c.detach(I2cBus::kRoot, Sht40Driver::kAddr);
    } else if (fault == "imu_no_selftest") {
      n.imu(1).self_test_mg = 0;
    } else if (fault == "sht_dead_heater") {
      n.sht(3).heater_rise_c = 0;
    } else if (fault.rfind("battery_mv=", 0) == 0) {
      n.adc.mv = static_cast<std::uint32_t>(std::strtoul(fault.c_str() + 11, nullptr, 10));
    } else {
      return false;
    }
    return true;
  }
  if (fault == "mux_nack") {
    n.i2c.mux_nack = true;
  } else if (fault == "expa_nack" && n.has_board_io()) {
    n.rev_a().exp_a.nack = true;
  } else if (fault == "expb_nack" && n.has_board_io()) {
    n.rev_a().exp_b.nack = true;
  } else if (fault == "charger_nack" && n.has_board_io()) {
    n.rev_a().charger.nack = true;
  } else if (fault == "expc_nack" && n.has_board_io()) {
    n.rev_a().exp_c.nack = true;
  } else if (fault == "lid_open" && n.has_board_io()) {
    n.rev_a().lid_open = true;  // J9 loop open: lid off or the wire cut
  } else if (fault == "lid_closed" && n.has_board_io()) {
    n.rev_a().lid_open = false;
  } else if (fault == "exp_int_stuck" && n.has_board_io()) {
    n.rev_a().exp_int_stuck = true;
  } else if (fault == "exp_wake_dead" && n.has_board_io()) {
    n.rev_a().exp_wake_dead = true;
  } else if (fault == "expc_bridge" && n.has_board_io()) {
    n.rev_a().exp_c.bridged_high = 1u << 10;  // EXP_C P12 (unused) bridged to V3V3
  } else if (slot_of("flipped", s)) {
    // Card inserted upside down (HD-025): it grounds PRESENT_N through the swapped
    // contact, loads no rail, and none of its devices line up with the slot's I2C.
    for (std::uint8_t addr = 0; addr < 0x80; ++addr)
      n.i2c.detach(n.board.slots[s].i2c_branch, addr);
  } else if (fault == "no_vin" && n.has_board_io()) {
    n.rev_a().charger.vin_good = false;
    n.rev_a().source = RevABoard::Source::kBatteryOnly;
  } else if (slot_of("branch_open", s)) {
    n.i2c.break_branch(n.board.slots[s].i2c_branch, true);
  } else if (slot_of("blank_id", s)) {
    n.id(s).blank();
  } else if (slot_of("no_id", s)) {
    n.i2c.detach(n.board.slots[s].i2c_branch, n.board.id_eeprom_address);
  } else if (slot_of("card_wake_open", s)) {
    n.card_wake_open[s] = true;
  } else if (slot_of("card_alert_open", s)) {
    n.card_alert_open[s] = true;
  } else if (slot_of("imu_no_selftest", s)) {
    n.imu(s).self_test_mg = 0;
  } else if (slot_of("sht_dead_heater", s)) {
    n.sht(s).heater_rise_c = 0;
  } else if (slot_of("sht_missing", s)) {
    n.i2c.detach(n.board.slots[s].i2c_branch, Sht40Driver::kAddr);
  } else {
    return false;
  }
  return true;
}

}  // namespace talos::fw::sim
