// Board plumbing behind the I/O expanders (TL-SN-001): slot rails, card
// presence, the power source and the BQ25186 charger. Implements IPower so the
// runtime keeps one interface; boards without expanders use a direct model.
//
// Safety rules enforced here (sensor_edge_v1 S-2/S-3, bench-test-plan.md §1):
// - Expander outputs are written before their pins are made outputs, so no
//   pin ever drives its power-on default (all ones) into a rail enable.
// - SW on only while AON is on; AON off only after SW is off.
// - CS_N and RESET_N are driven low while a slot's AON is off (no backfeed
//   through the card's pull-ups), and the mux never selects an unpowered slot.
// - The charger's push-button action is disabled (no button on this board), so
//   a shorted NTC can never send the node into ship mode.
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
  // Reads both input ports (clears INT). nullopt on NACK.
  std::optional<std::uint16_t> read_inputs();
  std::optional<std::uint16_t> read_register_pair(std::uint8_t reg);
  std::uint16_t outputs() const { return outputs_; }

 private:
  II2c& i2c_;
  std::uint8_t address_;
  std::uint16_t outputs_ = 0xFFFF;  // shadow: never read-modify-write from the input register
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

  BoardIo(II2c& i2c, IClock& clock, IBatteryAdc& adc, const BoardSupport& board);

  // IPower
  bool begin() override;
  void set_rail(std::uint8_t slot, Rail rail, bool on) override;
  std::uint32_t battery_mv() override;
  PowerSource source() override;
  std::optional<bool> slot_present(std::uint8_t slot) override;
  void service() override;
  std::uint8_t take_sw_faults() override;

  bool rail_on(std::uint8_t slot, Rail rail) const;
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

  II2c& i2c_;
  IClock& clock_;
  IBatteryAdc& adc_;
  const BoardSupport& board_;
  std::array<std::optional<Tca9535>, 2> expanders_;
  std::optional<Bq25186> charger_;
  std::array<std::array<bool, 2>, kSlots> rails_{};  // [slot][AON, SW]
  std::optional<std::uint8_t> uart_slot_;
  bool charger_ok_ = false;
  PowerSource last_source_ = PowerSource::kUnknown;
};

}  // namespace talos::fw
