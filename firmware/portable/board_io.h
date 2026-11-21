// Board plumbing behind the I/O expanders (TL-SN-001): slot rails, card
// presence, the power source and the BQ25186 charger. Implements IPower so the
// runtime keeps one interface; boards without expanders use a direct model.
//
// Safety rules enforced here (sensor_edge_v1 S-2/S-3, bench-test-plan.md §1):
// - Expander outputs are written before their pins are made outputs, so no
//   pin ever drives its power-on default (all ones) into a rail enable.
// - The output shadow starts at that safe pattern, not at the chip's POR value:
//   after an MCU warm reset the expander keeps its pins as outputs, and a
//   read-modify-write from an all-ones shadow would switch every rail on.
//   Rails and the UART mux are refused until the expanders took their safe
//   state (begin() is retried on the next request).
// - RESET_N and CS_N go high only kAonSettleMs after AON_EN, once the slot's
//   rail is up (TPS22917 tON with the fitted CT cap), so the card is never
//   backfed through its I/O clamps while its VDD ramps.
// - SW on only while AON is on; AON off only after SW is off.
// - CS_N and RESET_N are driven low while a slot's AON is off (no backfeed
//   through the card's pull-ups), and the mux never selects an unpowered slot.
// - The charger's push-button action is disabled (no button on this board), so
//   a shorted NTC can never send the node into ship mode.
// - Slot LEDs (EXP_C, active low) start dark; unused expander pins are outputs
//   driven low (TI guidance), never floating inputs.
// - The expanders' wired-OR INT wakes the MCU (EXP_WAKE). Every input read
//   releases that chip's INT, so a presence probe re-reads its port afterwards:
//   the probe's own edge must not leave INT asserted and wake the node again.
//
// Register maps follow TI SCPS201F (TCA9535) and SLUSF69A (BQ25186).
// PROVISIONAL until measured on the bench.
#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "firmware/portable/hal.h"

namespace talos::fw {

// TCA9535 16-bit I/O expander (no internal pull-ups; INT clears on reading the port).
class Tca9535 {
 public:
  static constexpr std::uint8_t kInput0 = 0x00, kOutput0 = 0x02, kPolarity0 = 0x04, kConfig0 = 0x06;
  Tca9535(II2c& i2c, std::uint8_t address) : i2c_(i2c), address_(address) {}
  std::uint8_t address() const { return address_; }
  // Writes both output ports, then both config ports (1 = input).
  bool configure(std::uint16_t outputs, std::uint16_t inputs_mask);
  bool write_outputs(std::uint16_t outputs);
  bool set_output(std::uint8_t pin, bool level);
  // Sets the shadow without bus traffic: the pattern the next read-modify-write starts from.
  void set_shadow(std::uint16_t outputs) { outputs_ = outputs; }
  // Reads both input ports (clears INT). nullopt on NACK.
  std::optional<std::uint16_t> read_inputs();
  std::optional<std::uint16_t> read_register_pair(std::uint8_t reg);
  std::uint16_t outputs() const { return outputs_; }

 private:
  II2c& i2c_;
  std::uint8_t address_;
  // Shadow: never read-modify-write from the input register. Not the POR value
  // (0xFFFF): BoardIo sets it to the board's safe pattern before any write.
  std::uint16_t outputs_ = 0x0000;
};

struct ChargerStatus {
  enum class Charge : std::uint8_t { kNotCharging, kConstantCurrent, kConstantVoltage, kDone };
  bool vin_good = false;
  Charge charge = Charge::kNotCharging;
  bool ts_open = false;
  std::uint8_t ts_zone = 0;  // TS_STAT: 0 normal, 1 cold/hot suspended, 2 cool, 3 warm
  bool thermal_regulation = false;
  bool battery_uvlo = false;
  bool vin_ovp = false;
  bool safety_timer_fault = false;
  std::uint8_t flags = 0;  // FLAG0, read-to-clear
};

// BQ25186 1-cell charger, configured over I2C only (no ILIM/ICHG resistors).
class Bq25186 {
 public:
  static constexpr std::uint8_t kStat0 = 0x00, kStat1 = 0x01, kFlag0 = 0x02, kVbatCtrl = 0x03,
                                kIchgCtrl = 0x04, kChargeCtrl0 = 0x05, kChargeCtrl1 = 0x06,
                                kIcCtrl = 0x07, kTmrIlim = 0x08, kShipRst = 0x09, kSysReg = 0x0A,
                                kTsControl = 0x0B, kMaskId = 0x0C;
  // ICHG code: <= 35 mA: code + 5 mA; > 35 mA: 40 + (code - 31) * 10 mA.
  static std::uint8_t ichg_code(std::uint32_t ma);
  // The full configuration for a given charge current (register, value), in write order.
  struct Write {
    std::uint8_t reg, value;
  };
  static std::array<Write, 9> configuration(std::uint32_t charge_ma);

  Bq25186(II2c& i2c, std::uint8_t address) : i2c_(i2c), address_(address) {}
  // Writes every field explicitly (the datasheet's reset bytes and field tables
  // disagree in places), then reads each back. False on NACK or mismatch.
  bool configure(std::uint32_t charge_ma);
  // True if the configuration still reads back (catches a register reset).
  bool verify();
  std::optional<ChargerStatus> status();
  std::uint32_t charge_ma() const { return charge_ma_; }

 private:
  bool read(std::uint8_t reg, std::uint8_t& out);
  II2c& i2c_;
  std::uint8_t address_;
  std::uint32_t charge_ma_ = 0;
};

class BoardIo final : public IPower {
 public:
  static constexpr std::uint32_t kUsbChargeMa = 300;  // power-tree.md
  static constexpr std::uint32_t kSolarChargeMa = 200;
  // AON_EN high to RESET_N/CS_N high. TPS22917 (SLVSDW8B §7.6) at VIN 3.6 V:
  // tON = 3.8 us/pF x C_CT, so ~3.8 ms typ with the fitted 1 nF (C27-C30); the
  // datasheet gives no max. ~2.6x typ. PROVISIONAL: scope it on the bench (L-13/L-14).
  static constexpr std::uint32_t kAonSettleMs = 10;

  BoardIo(II2c& i2c, IClock& clock, IBatteryAdc& adc, const BoardSupport& board);

  // IPower
  bool begin() override;
  bool resume(std::uint8_t aon_on, bool& board_ok) override;
  void set_rail(std::uint8_t slot, Rail rail, bool on) override;
  std::uint32_t battery_mv() override;
  PowerSource source() override;
  std::optional<bool> slot_present(std::uint8_t slot) override;
  void service() override;
  std::uint8_t take_sw_faults() override;
  std::optional<bool> tamper_open() override;
  bool set_slot_led(std::uint8_t slot, bool on) override;
  void service_interrupt() override;

  bool rail_on(std::uint8_t slot, Rail rail) const;
  // True once every expander took its safe state (begin() or a later retry).
  bool expanders_ready() const { return expanders_ready_; }
  bool charger_ok() const { return charger_ok_; }
  std::optional<ChargerStatus> charger_status();
  // UART mux (TS3A5017): route the card UART to one slot, or park it (nullopt).
  // Refused for an unpowered slot.
  bool select_uart(std::optional<std::uint8_t> slot);
  std::optional<std::uint8_t> uart_slot() const { return uart_slot_; }
  Tca9535* expander(std::uint8_t address);
  // Direct pin access for bench diagnostics (diag.h).
  bool drive(const ExpanderPin& p, bool level);
  std::optional<bool> sense(const ExpanderPin& p);
  const BoardSupport& board() const { return board_; }

 private:
  struct PinRef {
    Tca9535* chip = nullptr;
    std::uint8_t pin = 0;
  };
  PinRef ref(const ExpanderPin& p);
  // Writes the safe pattern to every expander (outputs, then directions).
  bool configure_expanders();
  void configure_charger();
  // Retries the expander configuration if begin() did not complete it.
  bool ensure_ready() { return expanders_ready_ || configure_expanders(); }
  struct Plan {
    std::uint16_t outputs = 0xFFFF;
    std::uint16_t output_mask = 0;  // 1 = driven by the expander
  };

  static constexpr std::size_t kMaxExpanders = 3;  // EXP_A, EXP_B, EXP_C
  II2c& i2c_;
  IClock& clock_;
  IBatteryAdc& adc_;
  const BoardSupport& board_;
  std::array<std::optional<Tca9535>, kMaxExpanders> expanders_;
  std::array<Plan, kMaxExpanders> safe_{};  // per expander, same order as expanders_
  // Slot LED pins per expander: their level is not part of the safe state, so
  // a lit LED never makes resume() distrust the board.
  std::array<std::uint16_t, kMaxExpanders> led_mask_{};
  bool expanders_ready_ = false;
  std::optional<Bq25186> charger_;
  std::array<std::array<bool, 2>, kSlots> rails_{};  // [slot][AON, SW]
  std::optional<std::uint8_t> uart_slot_;
  bool charger_ok_ = false;
  PowerSource last_source_ = PowerSource::kUnknown;
};

}  // namespace talos::fw
