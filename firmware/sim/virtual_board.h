// Virtual hardware for the portable firmware (spec §41, L2 twin): register-
// level models of the I2C mux, module ID EEPROMs, LIS2DW12, TMAG5273 and SHT40, plus
// GPIO, rails/battery, an atomic store with power-cut injection, A/B flash
// and a lossy loopback link. The firmware cannot tell it from hardware
// through the HAL; physical timing/electrical behavior stays in HIL (L4).
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "firmware/portable/board_io.h"
#include "firmware/portable/hal.h"
#include "firmware/portable/runtime.h"

namespace talos::fw::sim {

class Clock final : public IClock {
 public:
  TimeMs now_ms() const override { return t_; }
  // A blocking delay lets time pass inside a step, as it does on the chip.
  void delay_ms(std::uint32_t ms) override { t_ += ms; }
  void advance(TimeMs ms) { t_ += ms; }

 private:
  TimeMs t_ = 1'000;
};

class Gpio final : public IGpio {
 public:
  bool read(std::uint8_t line) override { return line < lines_.size() && lines_[line]; }
  void set(std::uint8_t line, bool v) { lines_.at(line) = v; }
  // Output lines (mux reset): the board model decides what is wired there.
  bool write(std::uint8_t line, bool level) override {
    if (!on_write) return false;
    on_write(line, level);
    return true;
  }
  std::function<void(std::uint8_t line, bool level)> on_write;

 private:
  std::array<bool, 32> lines_{};
};

class I2cDevice {
 public:
  virtual ~I2cDevice() = default;
  virtual bool acks() const { return true; }  // answers its address
  virtual bool write(const std::uint8_t* data, std::size_t n) = 0;
  virtual bool read(std::uint8_t* out, std::size_t n) = 0;
};

// TCA9546A-style mux at a fixed address; exactly one branch must be selected
// to reach a branch device. A broken branch fails every transaction on it;
// mux_nack makes the mux itself stop answering.
class I2cBus final : public II2c {
 public:
  static constexpr std::uint8_t kRoot = 0xFF;  // devices upstream of the mux
  explicit I2cBus(std::uint8_t mux_address) : mux_(mux_address) {}
  std::uint8_t mask() const { return mask_; }
  bool mux_nack = false;
  // Branch devices only answer while their slot is powered (AON). Unset = always.
  std::function<bool(std::uint8_t branch)> branch_powered;
  void attach(std::uint8_t branch, std::uint8_t addr, I2cDevice* dev) {
    devices_[{branch, addr}] = dev;
  }
  void detach(std::uint8_t branch, std::uint8_t addr) { devices_.erase({branch, addr}); }
  void break_branch(std::uint8_t branch, bool broken) { broken_[branch] = broken; }
  // A card holding SDA low: while its branch is selected, nothing on the bus answers.
  void stick_branch(std::uint8_t branch, bool stuck) { stuck_[branch] = stuck; }
  void reset_mux() { mask_ = 0; }  // TCA9546A RESET_N pulse
  std::uint32_t mux_resets = 0;
  bool write(std::uint8_t addr, const std::uint8_t* data, std::size_t n) override;
  bool write_read(std::uint8_t addr, const std::uint8_t* w, std::size_t wn, std::uint8_t* r,
                  std::size_t rn) override;
  bool probe(std::uint8_t addr) override;
  std::uint32_t transactions() const { return transactions_; }

 private:
  I2cDevice* target(std::uint8_t addr);
  bool held() const;
  std::array<bool, 8> stuck_{};
  std::uint8_t mux_;
  std::uint8_t mask_ = 0;
  std::map<std::pair<std::uint8_t, std::uint8_t>, I2cDevice*> devices_;
  std::array<bool, 8> broken_{};
  std::uint32_t transactions_ = 0;
};

class RegisterDevice : public I2cDevice {
 public:
  bool write(const std::uint8_t* data, std::size_t n) override;
  bool read(std::uint8_t* out, std::size_t n) override;
  std::array<std::uint8_t, 256> regs{};

 protected:
  // Called after a register's value has been returned (clear-on-read regs).
  virtual void after_read(std::uint8_t /*reg*/) {}
  std::uint8_t ptr_ = 0;
};

// 24AA025E48: ID block v1 in page 0, EUI-48 at 0xFA.
class IdEeprom final : public RegisterDevice {
 public:
  IdEeprom(ModuleCode code, std::uint8_t revision);
  void blank() { regs.fill(0xFF); }
};

// TCA9535 (SCPS201F): input registers read the pin level (the output latch for
// pins configured as outputs, the external level otherwise); POR outputs 0xFFFF,
// all pins inputs. `external` supplies the level of each pin when it is an input.
class Tca9535Model final : public RegisterDevice {
 public:
  Tca9535Model();
  bool read(std::uint8_t* out, std::size_t n) override;
  // Level the pin is at: the output latch if configured as output, else external(pin).
  bool level(std::uint8_t pin) const;
  bool is_output(std::uint8_t pin) const;
  void power_on_reset();
  std::function<bool(std::uint8_t pin)> external = [](std::uint8_t) { return true; };
  bool acks() const override { return !nack; }
  bool nack = false;  // fault: the expander stops answering
  bool write(const std::uint8_t* data, std::size_t n) override;
  std::function<void()> on_write;  // a pin may have changed: update what it drives
  // Every pin that has ever driven a 1 as an output (catches config-before-output).
  std::uint16_t ever_driven_high = 0;
};

// BQ25186 (SLUSF69A): reset values, STAT0/STAT1 from the modelled input and
// battery, FLAG0 read-to-clear, register reset (watchdog or brown-out).
class Bq25186Model final : public RegisterDevice {
 public:
  Bq25186Model();
  bool read(std::uint8_t* out, std::size_t n) override;
  void reset_registers();
  bool vin_good = true;
  bool ts_open = false;
  std::uint8_t ts_zone = 0;
  bool acks() const override { return !nack; }
  bool nack = false;
  bool write(const std::uint8_t* data, std::size_t n) override {
    return !nack && RegisterDevice::write(data, n);
  }
};

class Adc final : public IBatteryAdc {
 public:
  std::optional<std::uint32_t> battery_mv() override { return mv; }
  std::optional<std::uint32_t> mv = 3950;
};

// TL-SN-001 plumbing around the expanders: who powers what, what each input
// pin sees. Mirrors hardware/kicad/TL-SN-001 (pull-ups and pull-downs included).
class RevABoard {
 public:
  enum class Source : std::uint8_t { kUsb, kSolar, kBatteryOnly };
  explicit RevABoard(const BoardSupport& board);
  Tca9535Model exp_a, exp_b;
  Bq25186Model charger;
  std::array<bool, kSlots> seated{};       // a card grounds PRESENT_N
  std::array<bool, kSlots> alert_low{};    // a card pulls ALERT_N low
  std::array<bool, kSlots> sw_overload{};  // the card overloads SW: its switch asserts FAULT_N
  Source source = Source::kUsb;
  bool aon_on(std::uint8_t slot) const;  // TPS22917 ON level (1M pull-down when not driven)
  bool sw_on(std::uint8_t slot) const;   // 1M pull-down when not driven
  // Line level at the card for RESET_N / CS_N (pulled up to the slot's AON).
  bool reset_n(std::uint8_t slot) const;
  bool cs_n(std::uint8_t slot) const;

 private:
  bool expander_pin(const ExpanderPin& p, bool undriven) const;
  std::uint8_t exp_a_address() const { return board_.slot_io[0].aon_enable.address; }
  const BoardSupport& board_;
};

// LIS2DW12 (DS11811) as far as the driver depends on it: WHO_AM_I, soft reset,
// the wake-up threshold, and the interrupt path. INT1 only carries the
// wake-up event when CTRL4 routes it and CTRL7 INTERRUPTS_ENABLE is set; it is
// a pulse of one ODR period unless CTRL3 LIR latches it. Outputs carry 1 g of
// gravity on Z unless CTRL6 FDS selects the high-pass path.
class Lis2dw12 final : public RegisterDevice {
 public:
  Lis2dw12(Gpio& gpio, std::uint8_t int_line, const Clock& clock);
  // A physical impact reaching the sensor along X.
  void impact(std::int32_t mg);
  // Static acceleration (gravity) in mg; the low-pass path reports it, the
  // high-pass path (CTRL6 FDS) does not.
  std::array<std::int32_t, 3> static_mg{0, 0, 1000};
  // Output shift per axis while CTRL3 self-test is on (+ for ST=01, - for ST=10).
  std::int32_t self_test_mg = 400;
  bool read(std::uint8_t* out, std::size_t n) override;
  // Advance to the current time (drops a pulsed INT1 after one ODR period).
  void tick();
  bool write(const std::uint8_t* data, std::size_t n) override;
  bool int_stuck_high = false;  // fault: INT1 bridged to VDD
  int read_only_reg = -1;       // fault: writes to this register do not take

 protected:
  void after_read(std::uint8_t reg) override;

 private:
  void reset_registers();
  void set_int(bool v);
  Gpio& gpio_;
  std::uint8_t line_;
  const Clock& clock_;
  void load_outputs();
  TimeMs pulse_until_ = 0;
  bool pulsing_ = false;
  std::int32_t impact_x_mg_ = 0;
};

// TMAG5273A1 register model (SLYS045C): results at 820 LSB/mT, symmetric
// per-axis thresholds (THR_HYST = 1, zero = axis not checked), MAG_THR_DIR,
// wake-up-and-sleep mode, latched INT cleared by any transaction. `wake_line`
// is the card's WAKE: INT_N through the on-card inverter, so high = asserted.
class Tmag5273 final : public RegisterDevice {
 public:
  Tmag5273(Gpio& gpio, std::uint8_t wake_line, const Clock& clock);
  // The magnet moved: new field at the sensor, microtesla per axis.
  void set_field(std::int32_t x_ut, std::int32_t y_ut, std::int32_t z_ut);
  bool write(const std::uint8_t* data, std::size_t n) override;
  bool read(std::uint8_t* out, std::size_t n) override;
  bool in_wake_sleep() const { return (regs[0x01] & 0x03) == 0x03; }
  std::uint8_t mode() const { return regs[0x01] & 0x03; }
  bool never_converts = false;  // fault: RESULT_STATUS never sets

 private:
  // Converts only the channels MAG_CH_EN enables; the others keep stale results.
  void update_results();
  void evaluate();
  void settle_conversion();
  Gpio& gpio_;
  std::uint8_t line_;
  const Clock& clock_;
  TimeMs continuous_since_ = 0;
  std::array<std::int32_t, 3> field_ut_{};
};

// SHT40 (SHT4x datasheet): NACKs while a command is executing (high-precision
// measurement 8.3 ms max, soft reset and serial ~1 ms), then returns six bytes
// once. Supports measure (0xFD), soft reset (0x94) and serial number (0x89).
class Sht40 final : public I2cDevice {
 public:
  explicit Sht40(const Clock& clock) : clock_(clock) {}
  bool write(const std::uint8_t* data, std::size_t n) override;
  bool read(std::uint8_t* out, std::size_t n) override;
  double temperature_c = 21.5;
  double humidity_pct = 45.0;
  bool corrupt_crc = false;    // RH word CRC
  bool corrupt_t_crc = false;  // temperature word CRC
  TimeMs measure_ms = 8;       // high-precision conversion time
  std::uint32_t serial = 0x1234ABCD;
  double heater_rise_c = 12.0;  // 110 mW for 1 s (0x2F); 0 = dead heater

 private:
  void load(std::uint16_t a, std::uint16_t b);
  void load_measurement(double t_c);
  const Clock& clock_;
  std::array<std::uint8_t, 6> out_{};
  bool ready_ = false;
  TimeMs busy_until_ = 0;
};

class Power final : public IPower {
 public:
  void set_rail(std::uint8_t slot, Rail rail, bool on) override {
    rails_[slot][static_cast<int>(rail)] = on;
  }
  std::uint32_t battery_mv() override { return battery_mv_; }
  PowerSource source() override { return source_; }
  bool rail(std::uint8_t slot, Rail r) const { return rails_[slot][static_cast<int>(r)]; }
  std::uint32_t battery_mv_ = 3950;
  PowerSource source_ = PowerSource::kUsb;

 private:
  std::array<std::array<bool, 3>, kSlots> rails_{};
};

// Atomic per-key store; fail_puts simulates power lost before the write lands.
class Store final : public IStore {
 public:
  std::optional<std::size_t> get(StoreKey key, std::uint8_t* buf, std::size_t cap) override;
  bool put(StoreKey key, const std::uint8_t* data, std::size_t n) override;
  int fail_puts = 0;
  std::uint32_t puts = 0;

 private:
  std::map<StoreKey, std::vector<std::uint8_t>> kv_;
};

class Flash final : public IFlash {
 public:
  explicit Flash(std::uint32_t capacity)
      : slots_{std::vector<std::uint8_t>(capacity, 0xFF),
               std::vector<std::uint8_t>(capacity, 0xFF)} {}
  std::uint32_t slot_capacity() const override {
    return static_cast<std::uint32_t>(slots_[0].size());
  }
  bool erase(std::uint8_t slot) override;
  bool write(std::uint8_t slot, std::uint32_t offset, const std::uint8_t* data,
             std::size_t n) override;
  bool read(std::uint8_t slot, std::uint32_t offset, std::uint8_t* out, std::size_t n) override;

 private:
  std::array<std::vector<std::uint8_t>, 2> slots_;
};

class Link final : public ILink {
 public:
  bool send(const DeviceEvent& e) override;
  std::optional<Downlink> receive() override;
  bool connected = true;
  int drop_next = 0;               // drop this many uplink sends
  std::deque<DeviceEvent> uplink;  // what reached the hub side
  std::deque<Downlink> downlink;   // queued for the device
  std::uint32_t attempts = 0;
};

// A whole node: hardware models + BoardSupport + the real NodeRuntime.
class Node {
 public:
  // `link_override` replaces the in-memory Link (e.g. a CoapLink over UDP).
  explicit Node(BoardSupport board, std::uint32_t flash_capacity = 64 * 1024,
                ILink* link_override = nullptr);

  // Plug a module into a slot (ID EEPROM + its devices on the slot branch).
  void insert(std::uint8_t slot, ModuleCode code);
  Lis2dw12& imu(std::uint8_t slot) { return *imus_.at(slot); }
  IdEeprom& id(std::uint8_t slot) { return *eeproms_.at(slot); }
  Tca9535Model& test_card(std::uint8_t slot) { return *test_cards_.at(slot); }
  // Present on boards with expanders (tl_sn_001_board()): the real BoardIo runs
  // against these models and is the platform's IPower.
  RevABoard& rev_a() { return *rev_a_; }
  BoardIo& board_io() { return *board_io_; }
  bool has_board_io() const { return board_io_.has_value(); }
  void remove(std::uint8_t slot);
  // TL-TEST-001 contact faults: the card's WAKE / ALERT_N drive never reaches the board.
  std::array<bool, kSlots> card_wake_open{};
  std::array<bool, kSlots> card_alert_open{};
  Sht40& sht(std::uint8_t slot) { return *shts_.at(slot); }
  Tmag5273& hall(std::uint8_t slot) { return *halls_.at(slot); }

  void boot();  // power-on / reset: fresh runtime, same store and flash
  // Advances virtual time in 1 ms ticks, stepping the runtime when it is due
  // or a wake line changed; handles requested reboots.
  void run_for(TimeMs ms);
  // Hub stand-in: returns unique events from link.uplink (deduplicated by
  // boot_epoch/seq) and ACKs every received copy.
  std::vector<DeviceEvent> drain_and_ack();

  NodeRuntime& runtime() { return *runtime_; }
  Platform& platform() { return platform_; }
  // Step every tick so an external link's replies are picked up promptly.
  bool poll_link = false;
  // Step the way firmware/esp_idf/main.cc does: no interrupt on wake-line
  // edges, just a poll every min(next_wake - now, 20 ms). Pulses shorter than
  // the poll period are invisible unless a sensor latches them.
  bool esp_polling = false;
  std::uint32_t reboots() const { return reboots_; }

  Clock clock;
  Gpio gpio;
  I2cBus i2c;
  Power power;
  Adc adc;
  Store store;
  Flash flash;
  Link link;
  BoardSupport board;

 private:
  std::array<bool, 32> last_lines_{};
  TimeMs next_poll_ = 0;
  IPower& select_power();
  std::optional<RevABoard> rev_a_;
  std::optional<BoardIo> board_io_;
  Platform platform_;
  std::optional<NodeRuntime> runtime_;
  std::map<std::uint8_t, std::unique_ptr<IdEeprom>> eeproms_;
  std::map<std::uint8_t, std::unique_ptr<Lis2dw12>> imus_;
  std::map<std::uint8_t, std::unique_ptr<Sht40>> shts_;
  std::map<std::uint8_t, std::unique_ptr<Tmag5273>> halls_;
  std::map<std::uint8_t, std::unique_ptr<Tca9535Model>> test_cards_;
  std::uint32_t reboots_ = 0;
};

// Four-slot board shape from the provisional manifest (ADR 0013): slot i has
// wake line i, I2C branch i; only slot 2 has a 5 V rail. No expanders.
BoardSupport default_board();
// TL-SN-001 Rev A: the ESP build's board table (expanders, charger, presence).
BoardSupport tl_sn_001_board();

}  // namespace talos::fw::sim
