// Named faults for the virtual TL-SN-001, shared by bench_node and tests so the
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
  if (fault == "mux_nack") {
    n.i2c.mux_nack = true;
  } else if (fault == "expa_nack" && n.has_board_io()) {
    n.rev_a().exp_a.nack = true;
  } else if (fault == "expb_nack" && n.has_board_io()) {
    n.rev_a().exp_b.nack = true;
  } else if (fault == "charger_nack" && n.has_board_io()) {
    n.rev_a().charger.nack = true;
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
