// Virtual hardware for the portable firmware (spec §41, L2 twin): register-
// level models of the I2C mux, module ID EEPROMs, LIS2DW12, TMAG5273 and SHT40, plus
// GPIO, rails/battery, an atomic store with power-cut injection, A/B flash
// and a lossy loopback link. The firmware cannot tell it from hardware
// through the HAL; physical timing/electrical behavior stays in HIL (L4).
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "firmware/portable/hal.h"
#include "firmware/portable/runtime.h"

namespace talos::fw::sim {

class Clock final : public IClock {
 public:
  TimeMs now_ms() const override { return t_; }
  void advance(TimeMs ms) { t_ += ms; }

 private:
  TimeMs t_ = 1'000;
};

class Gpio final : public IGpio {
 public:
  bool read(std::uint8_t line) override { return line < lines_.size() && lines_[line]; }
  void set(std::uint8_t line, bool v) { lines_.at(line) = v; }

 private:
  std::array<bool, 32> lines_{};
};

class I2cDevice {
 public:
  virtual ~I2cDevice() = default;
  virtual bool write(const std::uint8_t* data, std::size_t n) = 0;
  virtual bool read(std::uint8_t* out, std::size_t n) = 0;
};

// TCA9546A-style mux at a fixed address; exactly one branch must be selected
// to reach a branch device. A broken branch fails every transaction on it.
class I2cBus final : public II2c {
 public:
  explicit I2cBus(std::uint8_t mux_address) : mux_(mux_address) {}
  void attach(std::uint8_t branch, std::uint8_t addr, I2cDevice* dev) {
    devices_[{branch, addr}] = dev;
  }
  void detach(std::uint8_t branch, std::uint8_t addr) { devices_.erase({branch, addr}); }
  void break_branch(std::uint8_t branch, bool broken) { broken_[branch] = broken; }
  bool write(std::uint8_t addr, const std::uint8_t* data, std::size_t n) override;
  bool write_read(std::uint8_t addr, const std::uint8_t* w, std::size_t wn, std::uint8_t* r,
                  std::size_t rn) override;
  std::uint32_t transactions() const { return transactions_; }

 private:
  I2cDevice* target(std::uint8_t addr);
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

class IdEeprom final : public RegisterDevice {
 public:
  IdEeprom(ModuleCode code, std::uint8_t revision);
};

class Lis2dw12 final : public RegisterDevice {
 public:
  Lis2dw12(Gpio& gpio, std::uint8_t int_line);
  // A physical impact reaching the sensor: latches a wake-up and raises INT1
  // when above the programmed threshold.
  void impact(std::int32_t mg);

 protected:
  void after_read(std::uint8_t reg) override;

 private:
  Gpio& gpio_;
  std::uint8_t line_;
};

// TMAG5273A1 register model (SLYS045C): results at 820 LSB/mT, symmetric
// per-axis thresholds (THR_HYST = 1, zero = axis not checked), MAG_THR_DIR,
// wake-up-and-sleep mode, latched INT cleared by any transaction. `wake_line`
// is the card's WAKE: INT_N through the on-card inverter, so high = asserted.
class Tmag5273 final : public RegisterDevice {
 public:
  Tmag5273(Gpio& gpio, std::uint8_t wake_line);
  // The magnet moved: new field at the sensor, microtesla per axis.
  void set_field(std::int32_t x_ut, std::int32_t y_ut, std::int32_t z_ut);
  bool write(const std::uint8_t* data, std::size_t n) override;
  bool read(std::uint8_t* out, std::size_t n) override;
  bool in_wake_sleep() const { return (regs[0x01] & 0x03) == 0x03; }

 private:
  void update_results();
  void evaluate();
  Gpio& gpio_;
  std::uint8_t line_;
  std::array<std::int32_t, 3> field_ut_{};
};

class Sht40 final : public I2cDevice {
 public:
  bool write(const std::uint8_t* data, std::size_t n) override;
  bool read(std::uint8_t* out, std::size_t n) override;
  double temperature_c = 21.5;
  double humidity_pct = 45.0;
  bool corrupt_crc = false;

 private:
  std::array<std::uint8_t, 6> out_{};
  bool ready_ = false;
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
  // Step every tick so an external link's replies are picked up promptly.
  bool poll_link = false;
  std::uint32_t reboots() const { return reboots_; }

  Clock clock;
  Gpio gpio;
  I2cBus i2c;
  Power power;
  Store store;
  Flash flash;
  Link link;
  BoardSupport board;

 private:
  std::array<bool, 32> last_lines_{};
  Platform platform_;
  std::optional<NodeRuntime> runtime_;
  std::map<std::uint8_t, std::unique_ptr<IdEeprom>> eeproms_;
  std::map<std::uint8_t, std::unique_ptr<Lis2dw12>> imus_;
  std::map<std::uint8_t, std::unique_ptr<Sht40>> shts_;
  std::map<std::uint8_t, std::unique_ptr<Tmag5273>> halls_;
  std::uint32_t reboots_ = 0;
};

// Four-slot board shape from the provisional manifest (ADR 0013): slot i has
// wake line i, I2C branch i; only slot 2 has a 5 V rail.
BoardSupport default_board();

}  // namespace talos::fw::sim
