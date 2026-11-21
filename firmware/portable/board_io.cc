#include "firmware/portable/board_io.h"

namespace talos::fw {

// --- TCA9535 ----------------------------------------------------------------------------

bool Tca9535::configure(std::uint16_t outputs, std::uint16_t inputs_mask) {
  // Output register first: at power-on it holds 0xFFFF, so making a pin an
  // output before writing it would drive a 1 (e.g. every SW rail on at once).
  if (!write_outputs(outputs)) return false;
  const std::uint8_t cfg[3] = {kConfig0, static_cast<std::uint8_t>(inputs_mask & 0xFF),
                               static_cast<std::uint8_t>(inputs_mask >> 8)};
  return i2c_.write(address_, cfg, 3);
}

bool Tca9535::write_outputs(std::uint16_t outputs) {
  const std::uint8_t out[3] = {kOutput0, static_cast<std::uint8_t>(outputs & 0xFF),
                               static_cast<std::uint8_t>(outputs >> 8)};
  if (!i2c_.write(address_, out, 3)) return false;
  outputs_ = outputs;
  return true;
}

bool Tca9535::set_output(std::uint8_t pin, bool level) {
  const auto bit = static_cast<std::uint16_t>(1u << pin);
  return write_outputs(level ? static_cast<std::uint16_t>(outputs_ | bit)
                             : static_cast<std::uint16_t>(outputs_ & ~bit));
}

std::optional<std::uint16_t> Tca9535::read_register_pair(std::uint8_t reg) {
  std::uint8_t r[2] = {};
  if (!i2c_.write_read(address_, &reg, 1, r, 2)) return std::nullopt;
  return static_cast<std::uint16_t>(r[0] | (r[1] << 8));
}

std::optional<std::uint16_t> Tca9535::read_inputs() { return read_register_pair(kInput0); }

// --- BQ25186 ----------------------------------------------------------------------------

std::uint8_t Bq25186::ichg_code(std::uint32_t ma) {
  if (ma <= 35) return static_cast<std::uint8_t>(ma < 5 ? 0 : ma - 5);
  std::uint32_t code = 31 + (ma - 40 + 9) / 10;  // round up to the next 10 mA step
  if (ma < 40) code = 31;
  if (code > 127) code = 127;
  return static_cast<std::uint8_t>(code);
}

std::array<Bq25186::Write, 9> Bq25186::configuration(std::uint32_t charge_ma) {
  return {{
      {kVbatCtrl, 0x46},  // PG pin = VIN power good; VBATREG 3.5 V + 70 x 10 mV = 4.20 V
      {kIchgCtrl, ichg_code(charge_ma)},  // charging enabled
      {kChargeCtrl0, 0x24},  // precharge 2x term, ITERM 10 %, VINDPM 4.5 V, thermal reg 100 C
      {kChargeCtrl1, 0x56},  // IBAT_OCP 1 A, BUVLO 3.0 V, CHG/ILIM IRQ masked, VINDPM IRQ on
      {kIcCtrl, 0x87},       // TS on, VLOWV 3 V, VRCH 100 mV, 6 h timer, watchdog disabled
      {kTmrIlim, 0x4D},      // MR long press 10 s (unused), autowake 1 s, ILIM 500 mA
      {kShipRst, 0x00},      // no push-button action, push button disabled: no ship mode from TS
      {kSysReg, 0x40},       // SYS 4.5 V, never pass-through (TPS63900 VIN max 5.5 V)
      {kTsControl, 0x00},    // JEITA defaults: 0/10/45/60 C, 0.5x ICHG when cool
  }};
}

bool Bq25186::read(std::uint8_t reg, std::uint8_t& out) {
  return i2c_.write_read(address_, &reg, 1, &out, 1);
}

bool Bq25186::configure(std::uint32_t charge_ma) {
  for (const auto& w : configuration(charge_ma)) {
    const std::uint8_t buf[2] = {w.reg, w.value};
    if (!i2c_.write(address_, buf, 2)) return false;
  }
  charge_ma_ = charge_ma;
  return verify();
}

bool Bq25186::verify() {
  for (const auto& w : configuration(charge_ma_)) {
    std::uint8_t got = 0;
    if (!read(w.reg, got) || got != w.value) return false;
  }
  return true;
}

std::optional<ChargerStatus> Bq25186::status() {
  // Single-register reads: burst auto-increment is not documented for this part.
  std::uint8_t s0 = 0, s1 = 0, f0 = 0;
  if (!read(kStat0, s0) || !read(kStat1, s1) || !read(kFlag0, f0)) return std::nullopt;
  ChargerStatus st;
  st.ts_open = (s0 & 0x80) != 0;
  st.charge = static_cast<ChargerStatus::Charge>((s0 >> 5) & 0x3);
  st.thermal_regulation = (s0 & 0x02) != 0;
  st.vin_good = (s0 & 0x01) != 0;
  st.vin_ovp = (s1 & 0x80) != 0;
  st.battery_uvlo = (s1 & 0x40) != 0;
  st.ts_zone = static_cast<std::uint8_t>((s1 >> 3) & 0x3);
  st.safety_timer_fault = (s1 & 0x04) != 0;
  st.flags = f0;
  return st;
}

// --- BoardIo ----------------------------------------------------------------------------

BoardIo::BoardIo(II2c& i2c, IClock& clock, IBatteryAdc& adc, const BoardSupport& board)
    : i2c_(i2c), clock_(clock), adc_(adc), board_(board) {
  auto add = [&](const ExpanderPin& p) {
    if (!p.wired() || expander(p.address)) return;
    for (auto& e : expanders_) {
      if (!e) {
        e.emplace(i2c_, p.address);
        return;
      }
    }
  };
  for (const auto& s : board_.slot_io) {
    for (const auto* p :
         {&s.aon_enable, &s.sw_enable, &s.reset_n, &s.cs_n, &s.present_n, &s.alert_n}) {
      add(*p);
    }
  }
  for (const auto* p : {&board_.present_probe, &board_.uart_sel0, &board_.uart_sel1,
                        &board_.uart_mux_en_n, &board_.charger_int_n, &board_.charger_pg_n,
                        &board_.source_st, &board_.sw_fault_n, &board_.tamper_n}) {
    add(*p);
  }
  for (const auto& p : board_.slot_led_n) add(p);
  for (const auto& u : board_.unused_pins) {
    if (u.address && u.mask) add(ExpanderPin{u.address, 0});
  }
  if (board_.charger_address) charger_.emplace(i2c_, board_.charger_address);

  // Safe state, per expander: outputs (level, is_output) for every driven pin;
  // everything else, including unused pins, stays an input.
  auto plan = [&](const ExpanderPin& p, bool level) {
    for (std::size_t i = 0; i < expanders_.size(); ++i) {
      if (expanders_[i] && p.wired() && expanders_[i]->address() == p.address) {
        const auto bit = static_cast<std::uint16_t>(1u << p.pin);
        safe_[i].output_mask = static_cast<std::uint16_t>(safe_[i].output_mask | bit);
        safe_[i].outputs = level ? static_cast<std::uint16_t>(safe_[i].outputs | bit)
                                 : static_cast<std::uint16_t>(safe_[i].outputs & ~bit);
      }
    }
  };
  for (const auto& s : board_.slot_io) {
    // Both rails match their hardware default (pull-downs: off). The runtime
    // powers seated slots one at a time during discovery, so a shorted card
    // can't take the whole node down at power-on.
    plan(s.aon_enable, false);
    plan(s.sw_enable, false);
    plan(s.reset_n, false);  // AON is off: card lines low, no backfeed through its pull-ups
    plan(s.cs_n, false);
  }
  plan(board_.present_probe, false);  // probe only while reading: saves 33 uA per card
  plan(board_.uart_mux_en_n, true);   // UART mux parked
  plan(board_.uart_sel0, false);
  plan(board_.uart_sel1, false);
  for (const auto& led : board_.slot_led_n) {
    plan(led, true);  // active low: dark
    for (std::size_t i = 0; i < expanders_.size(); ++i) {
      if (expanders_[i] && led.wired() && expanders_[i]->address() == led.address) {
        led_mask_[i] = static_cast<std::uint16_t>(led_mask_[i] | (1u << led.pin));
      }
    }
  }
  for (const auto& u : board_.unused_pins) {
    for (std::uint8_t pin = 0; pin < 16; ++pin) {
      if (u.address && (u.mask & (1u << pin))) plan(ExpanderPin{u.address, pin}, false);
    }
  }
  // TAMPER_N stays an input (1 MOhm pull-up, 10 nF: tau 10 ms): nothing to plan.
  // Until begin() writes it, a read-modify-write must start from the safe
  // pattern: the chip may still hold outputs from before an MCU reset.
  for (std::size_t i = 0; i < expanders_.size(); ++i) {
    if (expanders_[i]) expanders_[i]->set_shadow(safe_[i].outputs);
  }
}

Tca9535* BoardIo::expander(std::uint8_t address) {
  for (auto& e : expanders_) {
    if (e && e->address() == address) return &*e;
  }
  return nullptr;
}

BoardIo::PinRef BoardIo::ref(const ExpanderPin& p) {
  if (!p.wired()) return {};
  return {expander(p.address), p.pin};
}

bool BoardIo::drive(const ExpanderPin& p, bool level) {
  const PinRef r = ref(p);
  return r.chip && r.chip->set_output(r.pin, level);
}

std::optional<bool> BoardIo::sense(const ExpanderPin& p) {
  const PinRef r = ref(p);
  if (!r.chip) return std::nullopt;
  const auto in = r.chip->read_inputs();
  if (!in) return std::nullopt;
  return ((*in >> r.pin) & 1u) != 0;
}

bool BoardIo::configure_expanders() {
  bool ok = true;
  for (std::size_t i = 0; i < expanders_.size(); ++i) {
    if (!expanders_[i]) continue;
    ok &= expanders_[i]->configure(safe_[i].outputs,
                                   static_cast<std::uint16_t>(~safe_[i].output_mask));
    (void)expanders_[i]->read_inputs();  // baseline for INT: releases any change seen so far
  }
  expanders_ready_ = ok;
  return ok;
}

void BoardIo::configure_charger() {
  if (!charger_) return;
  last_source_ = source();
  charger_ok_ =
      charger_->configure(last_source_ == PowerSource::kUsb ? kUsbChargeMa : kSolarChargeMa);
}

bool BoardIo::begin() {
  for (auto& r : rails_) r = {false, false};
  uart_slot_.reset();
  bool ok = configure_expanders();
  configure_charger();
  if (charger_) ok &= charger_ok_;
  return ok;
}

bool BoardIo::resume(std::uint8_t aon_on, bool& board_ok) {
  // What the expanders must hold if only the AON rails in aon_on were on: the
  // safe pattern plus AON_EN, RESET_N and CS_N high for those slots (set_rail).
  std::array<std::uint16_t, kMaxExpanders> want{};
  for (std::size_t i = 0; i < expanders_.size(); ++i) want[i] = safe_[i].outputs;
  auto set = [&](const ExpanderPin& p) {
    for (std::size_t i = 0; i < expanders_.size(); ++i) {
      if (expanders_[i] && p.wired() && expanders_[i]->address() == p.address) {
        want[i] = static_cast<std::uint16_t>(want[i] | (1u << p.pin));
      }
    }
  };
  for (std::uint8_t s = 0; s < kSlots; ++s) {
    if (!(aon_on & (1u << s))) continue;
    const auto& io = board_.slot_io[s];
    set(io.aon_enable);
    set(io.reset_n);
    set(io.cs_n);
  }
  // Read back outputs and directions. Anything else (a SW rail, the UART mux,
  // a chip that reset) means the state is not what we think: refuse.
  for (std::size_t i = 0; i < expanders_.size(); ++i) {
    if (!expanders_[i]) continue;
    const auto outs = expanders_[i]->read_register_pair(Tca9535::kOutput0);
    const auto cfg = expanders_[i]->read_register_pair(Tca9535::kConfig0);
    // Slot LEDs may be lit (external power, service mode): adopt them as they are.
    const auto mask = static_cast<std::uint16_t>(safe_[i].output_mask & ~led_mask_[i]);
    if (!outs || !cfg || *cfg != static_cast<std::uint16_t>(~safe_[i].output_mask) ||
        (*outs & mask) != (want[i] & mask)) {
      return false;
    }
    want[i] = static_cast<std::uint16_t>((want[i] & ~led_mask_[i]) | (*outs & led_mask_[i]));
  }
  for (std::size_t i = 0; i < expanders_.size(); ++i) {
    if (expanders_[i]) expanders_[i]->set_shadow(want[i]);
  }
  expanders_ready_ = true;
  uart_slot_.reset();
  for (std::uint8_t s = 0; s < kSlots; ++s) rails_[s] = {(aon_on & (1u << s)) != 0, false};
  configure_charger();
  board_ok = !charger_ || charger_ok_;
  return true;
}

void BoardIo::set_rail(std::uint8_t slot, Rail rail, bool on) {
  if (slot >= kSlots) return;
  const auto& s = board_.slot_io[slot];
  auto& state = rails_[slot];
  // Nothing turns on until the expanders hold a known state (off is always allowed).
  if (on && !ensure_ready()) return;
  if (rail == Rail::kSwitched3v3) {
    if (on && !state[0]) return;  // S-2: SW only while AON is on
    if (drive(s.sw_enable, on)) state[1] = on;
    return;
  }
  if (rail != Rail::kAlwaysOn3v3) return;  // Rev A has no 5 V rail
  if (on) {
    if (state[0]) return;  // already on (and its card lines already up): nothing to switch
    if (!drive(s.aon_enable, true)) return;
    state[0] = true;
    clock_.delay_ms(kAonSettleMs);  // rail up before the card's lines go high (no backfeed)
    drive(s.reset_n, true);
    drive(s.cs_n, true);
    return;
  }
  // Off: SW first, then the card's lines low (no backfeed through its pull-ups).
  if (state[1]) set_rail(slot, Rail::kSwitched3v3, false);
  if (uart_slot_ == slot) select_uart(std::nullopt);
  drive(s.cs_n, false);
  drive(s.reset_n, false);
  if (drive(s.aon_enable, false)) state[0] = false;
}

bool BoardIo::rail_on(std::uint8_t slot, Rail rail) const {
  if (slot >= kSlots) return false;
  if (rail == Rail::kAlwaysOn3v3) return rails_[slot][0];
  if (rail == Rail::kSwitched3v3) return rails_[slot][1];
  return false;
}

std::uint32_t BoardIo::battery_mv() {
  const auto mv = adc_.battery_mv();
  return mv ? *mv : 0;  // 0 = unknown: the runtime treats it as critical, never as healthy
}

PowerSource BoardIo::source() {
  const auto pg = ref(board_.charger_pg_n);
  const auto st = ref(board_.source_st);
  if (!pg.chip || !st.chip || pg.chip != st.chip) return PowerSource::kUnknown;
  const auto in = pg.chip->read_inputs();
  if (!in) return PowerSource::kUnknown;
  const bool pg_n = (*in >> pg.pin) & 1u;
  const bool st_usb = (*in >> st.pin) & 1u;    // TPS2121 ST high = IN1 (USB) selected
  if (pg_n) return PowerSource::kBatteryOnly;  // charger sees no input
  return st_usb ? PowerSource::kUsb : PowerSource::kSolarBattery;
}

std::optional<bool> BoardIo::slot_present(std::uint8_t slot) {
  if (slot >= kSlots || !board_.slot_io[slot].present_n.wired() || !board_.present_probe.wired()) {
    return std::nullopt;
  }
  if (!drive(board_.present_probe, true)) return std::nullopt;
  const auto level = sense(board_.slot_io[slot].present_n);
  drive(board_.present_probe, false);
  // The probe falling moves every empty slot's PRESENT_N: read the port again so
  // that edge does not hold EXP_INT_N asserted (it would wake the node at once).
  if (auto* chip = ref(board_.slot_io[slot].present_n).chip) (void)chip->read_inputs();
  if (!level) return std::nullopt;
  return !*level;  // the card ties PRESENT_N to GND
}

std::optional<ChargerStatus> BoardIo::charger_status() {
  if (!charger_) return std::nullopt;
  return charger_->status();
}

void BoardIo::service() {
  if (!charger_) return;
  const PowerSource src = source();
  const std::uint32_t want = src == PowerSource::kUsb ? kUsbChargeMa : kSolarChargeMa;
  // Re-apply on a register reset (watchdog, brown-out) or a source change.
  if (src != last_source_ || want != charger_->charge_ma() || !charger_->verify()) {
    charger_ok_ = charger_->configure(want);
  }
  last_source_ = src;
}

std::uint8_t BoardIo::take_sw_faults() {
  std::uint8_t on = 0;
  for (std::uint8_t i = 0; i < kSlots; ++i)
    on = static_cast<std::uint8_t>(on | (rails_[i][1] << i));
  if (!on || !board_.sw_fault_n.wired())
    return 0;  // no SW rail on: nothing can trip, no bus traffic
  const auto level = sense(board_.sw_fault_n);
  if (!level || *level) return 0;
  // FAULT_N is wired-OR across the four switches, so the slot can't be told
  // apart: every SW rail that was on goes off, and each one is reported.
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    if (on & (1u << i)) set_rail(i, Rail::kSwitched3v3, false);
  }
  return on;
}

std::optional<bool> BoardIo::tamper_open() {
  if (!board_.tamper_n.wired()) return std::nullopt;
  const auto level = sense(board_.tamper_n);  // also releases EXP_C's INT
  if (!level) return std::nullopt;
  return *level;  // 1 MOhm pull-up: high = lid open or loop cut
}

bool BoardIo::set_slot_led(std::uint8_t slot, bool on) {
  if (slot >= kSlots || !board_.slot_led_n[slot].wired() || !ensure_ready()) return false;
  return drive(board_.slot_led_n[slot], !on);  // active low
}

void BoardIo::service_interrupt() {
  // TCA9535: INT clears when the port that changed is read; read both ports of each chip.
  for (auto& e : expanders_) {
    if (e) (void)e->read_inputs();
  }
}

bool BoardIo::select_uart(std::optional<std::uint8_t> slot) {
  if (!board_.uart_mux_en_n.wired()) return false;
  if (slot && (*slot >= kSlots || !rails_[*slot][0])) return false;  // S-3: never an unpowered slot
  if (!ensure_ready()) return false;
  // Disable, change the select lines, then enable: no glitch onto another slot.
  if (!drive(board_.uart_mux_en_n, true)) return false;
  uart_slot_.reset();
  if (!slot) return drive(board_.uart_sel0, false) && drive(board_.uart_sel1, false);
  if (!drive(board_.uart_sel0, (*slot & 1u) != 0) || !drive(board_.uart_sel1, (*slot & 2u) != 0) ||
      !drive(board_.uart_mux_en_n, false)) {
    return false;
  }
  uart_slot_ = slot;
  return true;
}

}  // namespace talos::fw
