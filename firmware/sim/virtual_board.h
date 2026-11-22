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

// The ESP task watchdog: Node resets the runtime (a reboot) when feed() has
// not been called for timeout_ms while the ESP-style polling loop runs.
class Watchdog final : public IWatchdog {
 public:
  explicit Watchdog(const Clock& clock) : clock_(clock) {}
  void feed() override {
    ++feeds;
    const TimeMs now = clock_.now_ms();
    if (now - last_feed_ms > max_gap_ms) max_gap_ms = now - last_feed_ms;
    last_feed_ms = now;
  }
  // Longest stretch without a feed: a blocking call inside one step (a flash
  // erase) starves the real task watchdog even if the step feeds afterwards.
  TimeMs max_gap_ms = 0;
  TimeMs timeout_ms = 5'000;  // firmware/esp_idf/main/main.cc kWatchdogTimeoutMs
  std::uint32_t feeds = 0;
  std::uint32_t bites = 0;
  TimeMs last_feed_ms = 0;

 private:
  const Clock& clock_;
};

class Gpio final : public IGpio {
 public:
  bool read(std::uint8_t line) override {
    if (line >= lines_.size() || force_low[line]) return false;
    if (live) {
      if (const auto v = live(line)) return *v;
    }
    return lines_[line];
  }
  // Lines driven by a board model (EXP_WAKE): read live, nullopt = not this line.
  std::function<std::optional<bool>(std::uint8_t line)> live;
  // Fault: the line never reaches the MCU high (a TVS failed short, an open contact).
  std::array<bool, 32> force_low{};
  void set(std::uint8_t line, bool v) { lines_.at(line) = v; }
  // Output lines (mux reset, status LED): the board model decides what is
  // wired there. Lines the board does not declare as outputs are refused.
  bool write(std::uint8_t line, bool level) override {
    if (line >= 32 || !(outputs & (1u << line))) return false;
    driven[line] = level;
    ++writes[line];
    if (on_write) on_write(line, level);
    return true;
  }
  std::function<void(std::uint8_t line, bool level)> on_write;
  std::uint32_t outputs = 0;    // bit n: GPIO n is an output on this board
  std::array<bool, 32> driven;  // last level written per output line
  std::array<std::uint32_t, 32> writes{};
  bool is_output(std::uint8_t line) const { return line < 32 && (outputs & (1u << line)); }

  Gpio() { driven.fill(true); }

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
  // Glitch injection: the Nth transaction from now (0 = the next) fails, once.
  int fail_in = -1;

 private:
  bool glitch() { return fail_in >= 0 && fail_in-- == 0; }
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
  // Fault: pins held high from outside (a solder bridge to V3V3) whatever drives them.
  std::uint16_t bridged_high = 0;
  // INT (open drain, active low): asserted while an input pin differs from the
  // value it had when its port was last read; reading the port releases it.
  bool int_asserted() const;

 protected:
  void after_read(std::uint8_t reg) override;

 private:
  std::uint16_t snapshot_ = 0xFFFF;  // input levels at the last read of each port
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
  // With a clock, each AON rail turns on aon_rise_ms after its enable goes
  // high (TPS22917 tON with the fitted 1 nF CT cap); without one, instantly.
  explicit RevABoard(const BoardSupport& board, const Clock* clock = nullptr);
  TimeMs aon_rise_ms = 4;  // SLVSDW8B: 3.8 us/pF x 1 nF typ at VIN 3.6 V
  // A card line (RESET_N / CS_N) was driven high while the slot's AON was not
  // up yet: the card was backfed through its I/O clamps.
  std::array<bool, kSlots> backfed{};
  Tca9535Model exp_a, exp_b, exp_c;
  Bq25186Model charger;
  bool lid_open = false;       // J9 tamper loop open (lid off, or the wire cut): TAMPER_N high
  bool exp_int_stuck = false;  // fault: EXP_INT_N held low (a dead expander, a solder bridge)
  bool exp_wake_dead = false;  // fault: EXP_WAKE never rises (inverter U19 dead or unpowered)
  // EXP_WAKE at the MCU: the wired-OR INT_N through the SN74AUP1G04, active high.
  bool exp_wake() const;
  // Slot LED S0..S3 lit (EXP_C P0n driven low; 2.2k to V3V3_CORE).
  bool slot_led(std::uint8_t slot) const;
  std::array<bool, kSlots> seated{};       // a card grounds PRESENT_N
  std::array<bool, kSlots> alert_low{};    // a card pulls ALERT_N low
  std::array<bool, kSlots> sw_overload{};  // the card overloads SW: its switch asserts FAULT_N
  std::array<std::uint32_t, kSlots> aon_cuts{};  // times each slot's AON went from on to off
  Source source = Source::kUsb;
  bool aon_on(std::uint8_t slot) const;  // TPS22917 ON level (1M pull-down when not driven)
  bool sw_on(std::uint8_t slot) const;   // 1M pull-down when not driven
  // Line level at the card for RESET_N / CS_N (pulled up to the slot's AON).
  bool reset_n(std::uint8_t slot) const;
  bool cs_n(std::uint8_t slot) const;

 private:
  bool expander_pin(const ExpanderPin& p, bool undriven) const;
  void on_exp_a_write();
  std::uint8_t exp_a_address() const { return board_.slot_io[0].aon_enable.address; }
  const BoardSupport& board_;
  const Clock* clock_;
  std::array<std::optional<TimeMs>, kSlots> aon_enabled_at_{};
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
  static constexpr TimeMs kImpactVisibleMs = 200;
  TimeMs impact_until_ = 0;
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
  // range_mt / lsb_per_mt: 40 / 820 for the A1 (TL-MOD-001), 133 / 246 for the
  // TMAG5273A2 built into TL-SN-002.
  Tmag5273(Gpio& gpio, std::uint8_t wake_line, const Clock& clock, std::int32_t range_mt = 40,
           std::int32_t lsb_per_mt = Tmag5273ContactDriver::kLsbPerMt);
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
  std::int32_t range_mt_;
  std::int32_t lsb_per_mt_;
  TimeMs continuous_since_ = 0;
  std::array<std::int32_t, 3> field_ut_{};
};

// TL-SN-002 Rev A (ADR 0019): one TCA9535 for eight lamps and PIR_PWR, built-in
// sensors on the root bus, direct GPIO inputs (tamper, USER, USB_PRESENT).
// Mirrors firmware/esp_idf/main/board_tl_sn_002.h (pull resistors included).
class Sn002Board {
 public:
  explicit Sn002Board(const BoardSupport& board);
  Tca9535Model exp;
  bool lid_open = false;  // SW4 lid switch open, or the loop cut: TAMPER high
  // SW5 battery door open (HD-049): TAMPER high too (in series), and
  // TAMPER_DOOR (EXP P11, 4.7M pull-up) high.
  bool battery_door_open = false;
  bool tamper_line() const { return lid_open || battery_door_open; }
  bool usb = true;              // VBUS present: USB_PRESENT high, power mux on USB
  bool user_pressed = false;    // USER button (to 3V3): high while pressed
  bool pir_out = false;         // what the PIR's output would do if powered
  bool pir_stuck_high = false;  // fault: PIR output stuck high whenever powered
  // PIR_PWR (EXP P10 driven high).
  bool pir_powered() const;
  // Lamp lit (its expander pin an output driven low; 2.2k to 3V3).
  bool lamp(Lamp l) const;
  bool sensor_led(std::uint8_t slot) const;
  std::uint32_t pir_power_cycles = 0;  // PIR_PWR high -> low transitions

 private:
  bool pin_lit(const ExpanderPin& p) const;
  const BoardSupport& board_;
  bool pir_was_on_ = false;
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
  std::map<StoreKey, std::uint32_t> puts_by_key;
  // ESP-IDF erasing the working NVS partition (NO_FREE_PAGES / NEW_VERSION_FOUND):
  // the permanent partition (boot epoch, identity) is a different partition and survives.
  void erase_working_partition();

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
  bool mark_running_valid() override {
    ++valid_marks;
    return !fail_mark_valid;
  }
  std::uint32_t block_size() const override { return kBlock; }
  bool erase_block(std::uint8_t slot, std::uint32_t index) override;
  std::uint32_t valid_marks = 0;  // bootloader told the running image is good
  bool fail_mark_valid = false;
  // NOR erase time: blocks the calling task (4 KiB sector erase, 45 ms typ).
  static constexpr std::uint32_t kBlock = 4096;
  Clock* clock = nullptr;
  TimeMs erase_ms_per_block = 0;

 private:
  std::array<std::vector<std::uint8_t>, 2> slots_;
};

// Deep-sleep wake info and LP RAM. Node::wake_from_deep_sleep() sets `info`;
// the masks record what the runtime armed.
class Sleep final : public ISleep {
 public:
  WakeInfo wake_info() override { return info; }
  bool set_wake_enabled(std::uint8_t line, bool enabled) override {
    if (line >= 32) return false;
    const auto bit = 1u << line;
    masked = enabled ? (masked & ~bit) : (masked | bit);
    return true;
  }
  std::uint8_t* retained() override { return ram.data(); }
  std::size_t retained_size() const override { return ram.size(); }
  bool wake_enabled(std::uint8_t line) const { return (masked & (1u << line)) == 0; }
  WakeInfo info;
  std::uint32_t masked = 0;
  std::array<std::uint8_t, 256> ram{};
};

class Link final : public ILink {
 public:
  bool send(const DeviceEvent& e) override;
  std::optional<Downlink> receive() override;
  bool set_tx_power_dbm(std::int8_t dbm) override {
    tx_power_dbm = dbm;
    return true;
  }
  std::int8_t tx_power_dbm = 0x7F;  // last cap the runtime set (0x7F = never set)
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
  // Present on TL-SN-002 (tl_sn_002_board()).
  Sn002Board& sn002() { return *sn002_; }
  bool is_sn002() const { return sn002_.has_value(); }
  // Drive a sensor's output line: on TL-SN-002 the PIR line only follows while
  // PIR_PWR is on; everything else is a plain GPIO level.
  void set_line(std::uint8_t line, bool level);
  BoardIo& board_io() { return *board_io_; }
  bool has_board_io() const { return board_io_.has_value(); }
  void remove(std::uint8_t slot);
  // TL-TEST-001 contact faults: the card's WAKE / ALERT_N drive never reaches the board.
  std::array<bool, kSlots> card_wake_open{};
  std::array<bool, kSlots> card_alert_open{};
  Sht40& sht(std::uint8_t slot) { return *shts_.at(slot); }
  Tmag5273& hall(std::uint8_t slot) { return *halls_.at(slot); }

  // MCU reset: fresh runtime and BoardIo (RAM), same store and flash; the
  // expanders are not reset (they sit on V3V3_CORE) and keep their registers.
  void boot();
  // ESP32-H2 deep-sleep wake: also a reset into boot(), but LP RAM, the
  // expanders and every powered card kept running. `lines`: the wake lines
  // that fired (bit n = GPIO n); 0 = a timer wake.
  void wake_from_deep_sleep(std::uint32_t lines);

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
  Watchdog watchdog{clock};
  Gpio gpio;
  I2cBus i2c;
  Power power;
  Adc adc;
  Store store;
  Flash flash;
  Link link;
  Sleep sleep;
  BoardSupport board;

 private:
  std::array<bool, 32> last_lines_{};
  TimeMs next_poll_ = 0;
  IPower& select_power();
  void attach_built_ins();
  // Declared before platform_: select_power() (in platform_'s initializer) fills them on TL-SN-002.
  std::map<std::uint8_t, std::unique_ptr<IdEeprom>> eeproms_;
  std::map<std::uint8_t, std::unique_ptr<Lis2dw12>> imus_;
  std::map<std::uint8_t, std::unique_ptr<Sht40>> shts_;
  std::map<std::uint8_t, std::unique_ptr<Tmag5273>> halls_;
  std::map<std::uint8_t, std::unique_ptr<Tca9535Model>> test_cards_;
  std::optional<RevABoard> rev_a_;
  std::optional<Sn002Board> sn002_;
  std::optional<BoardIo> board_io_;
  Platform platform_;
  std::optional<NodeRuntime> runtime_;
  std::uint32_t reboots_ = 0;
};

// Four-slot board shape from the provisional manifest (ADR 0013): slot i has
// wake line i, I2C branch i; only slot 2 has a 5 V rail. No expanders.
BoardSupport default_board();
// TL-SN-001 Rev A: the ESP build's board table (expanders, charger, presence).
BoardSupport tl_sn_001_board();
// TL-SN-002 Rev A (ADR 0019): built-in sensors, one expander, direct lines.
BoardSupport tl_sn_002_board();

}  // namespace talos::fw::sim
