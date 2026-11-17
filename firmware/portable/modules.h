// Module registry, slot discovery and drivers (spec §31-32).
//
// Every module carries an ID EEPROM on its slot's I2C branch (2-byte module
// code + revision). Discovery reads it and picks a driver from the static
// registry; the board's SlotWiring decides whether the slot can host it.
// Register maps follow the datasheets as understood today and are
// PROVISIONAL until validated on hardware (ADR 0013).
#pragma once

#include <cstdint>
#include <variant>

#include "firmware/portable/hal.h"
#include "firmware/portable/types.h"

namespace talos::fw {

enum class ModuleCode : std::uint16_t {
  kNone = 0,
  kPirEkmb = 0x0101,
  kReedContact = 0x0102,
  kTmag5273Contact = 0x0103,  // TL-MOD-001: 3-axis Hall, orientation-independent
  kLis2dw12 = 0x0201,
  kSht40 = 0x0301,
  kSmartCameraS3 = 0x0401,
};

struct ModuleInfo {
  ModuleCode code;
  const char* manifest_id;  // matches contracts/manifests/modules/*.yaml
  bool needs_wake;
  bool needs_5v;
  bool critical;  // intrusion sensing: never disabled for power
  std::uint32_t average_mw;
};

const ModuleInfo* find_module(ModuleCode code);

// Runtime-tunable behavior (ConfigUpdate; persisted).
struct RuntimeConfig {
  std::uint32_t revision = 0;
  std::uint32_t heartbeat_interval_ms = 60'000;
  std::uint32_t environment_interval_ms = 300'000;
  std::uint32_t impact_threshold_mg = 600;
};

class IEventSink {
 public:
  virtual ~IEventSink() = default;
  virtual void emit(std::uint8_t slot, EventKind kind, Priority priority, bool flag, std::int32_t a,
                    std::uint32_t b) = 0;
};

// Per-slot access to the platform: selects the slot's mux branch before I2C.
class SlotIo {
 public:
  SlotIo(Platform& p, std::uint8_t slot) : p_(p), slot_(slot) {}
  std::uint8_t slot() const { return slot_; }
  const SlotWiring& wiring() const { return p_.board.slots[slot_]; }
  bool select_branch();
  bool read_wake_line();
  bool reg_read(std::uint8_t addr, std::uint8_t reg, std::uint8_t* out, std::size_t n);
  bool reg_write(std::uint8_t addr, std::uint8_t reg, std::uint8_t value);
  bool raw_write(std::uint8_t addr, const std::uint8_t* data, std::size_t n);
  bool raw_read(std::uint8_t addr, std::uint8_t* out, std::size_t n);

 private:
  Platform& p_;
  std::uint8_t slot_;
};

// --- Drivers ----------------------------------------------------------------------------

class PirDriver {
 public:
  bool init(SlotIo& io, IEventSink& sink, TimeMs now);
  void poll(SlotIo& io, IEventSink& sink, TimeMs now);
  bool busy() const { return false; }

 private:
  bool last_ = false;
};

// Reed/Hall contact with time-based debounce: a level must hold for
// kDebounceMs before it becomes the reported state (spec §3 bounce example).
class ContactDriver {
 public:
  static constexpr TimeMs kDebounceMs = 20;
  bool init(SlotIo& io, IEventSink& sink, TimeMs now);
  void poll(SlotIo& io, IEventSink& sink, TimeMs now);
  bool busy() const { return candidate_ != stable_; }

 private:
  bool stable_ = false;
  bool candidate_ = false;
  TimeMs since_ = 0;
};

// TMAG5273A1 3-axis Hall door contact (TL-MOD-001, TI SLYS045C). The sensor runs
// autonomously in wake-up-and-sleep mode; its open-drain INT (inverted on the
// card to an idle-low push-pull WAKE) latches on a threshold cross and drops the
// sensor to standby until this driver reads it and re-arms.
//   closed: only the dominant axis is checked, interrupt when |B| < kOpenUt
//   open:   all three axes are checked, interrupt when any |B| > kCloseUt
// so the magnet can sit at any angle. Config is volatile: init() runs after
// every power-up.
class Tmag5273ContactDriver {
 public:
  static constexpr std::uint8_t kAddr = 0x35;  // TMAG5273A1 factory address
  static constexpr std::uint8_t kDeviceConfig2 = 0x01, kSensorConfig1 = 0x02;
  static constexpr std::uint8_t kSensorConfig2 = 0x03, kXThr = 0x04, kIntConfig1 = 0x08;
  static constexpr std::uint8_t kManufacturerIdLsb = 0x0E, kXMsbResult = 0x12;
  static constexpr std::uint16_t kManufacturerId = 0x5449;  // "TI"
  static constexpr std::uint8_t kSleep100Ms = 0x07;
  static constexpr std::int32_t kOpenUt = 1'500;   // door counts as open below 1.5 mT
  static constexpr std::int32_t kCloseUt = 3'000;  // and closed above 3 mT (hysteresis)
  static constexpr std::int32_t kLsbPerMt = 820;   // +/-40 mT range
  bool init(SlotIo& io, IEventSink& sink, TimeMs now);
  void poll(SlotIo& io, IEventSink& sink, TimeMs now);
  bool busy() const { return false; }
  // 8-bit threshold code for +/-40 mT range: 40 mT / 128 per code, rounded up.
  static std::uint8_t threshold_code(std::int32_t ut);
  bool open() const { return open_; }

 private:
  bool read_field(SlotIo& io, std::int32_t (&ut)[3]);
  bool arm(SlotIo& io, const std::int32_t (&ut)[3]);
  bool open_ = true;
};

class Lis2dw12Driver {
 public:
  static constexpr std::uint8_t kAddr = 0x19;
  static constexpr std::uint8_t kWhoAmI = 0x0F, kWhoAmIValue = 0x44;
  static constexpr std::uint8_t kCtrl1 = 0x20, kCtrl4Int1 = 0x23, kCtrl6 = 0x25;
  static constexpr std::uint8_t kOutXL = 0x28, kWakeUpThs = 0x34, kWakeUpSrc = 0x38;
  bool init(SlotIo& io, IEventSink& sink, TimeMs now, std::uint32_t threshold_mg);
  void poll(SlotIo& io, IEventSink& sink, TimeMs now);
  bool set_threshold(SlotIo& io, std::uint32_t threshold_mg);
  bool busy() const { return false; }
  static std::uint8_t threshold_register(std::uint32_t threshold_mg);
};

class Sht40Driver {
 public:
  static constexpr std::uint8_t kAddr = 0x44;
  static constexpr std::uint8_t kMeasureHighPrecision = 0xFD;
  bool init(SlotIo& io, IEventSink& sink, TimeMs now);
  void poll(SlotIo& io, IEventSink& sink, TimeMs now, std::uint32_t interval_ms);
  bool busy() const { return false; }
  TimeMs next_at() const { return next_at_; }
  static std::uint8_t crc8(const std::uint8_t* data, std::size_t n);

 private:
  TimeMs next_at_ = 0;
};

// Intelligent module (camera): presence and power only; it has its own
// processor, radio and OTA (spec §33).
class SmartModuleDriver {
 public:
  bool init(SlotIo&, IEventSink&, TimeMs) { return true; }
  void poll(SlotIo&, IEventSink&, TimeMs) {}
  bool busy() const { return false; }
};

using Driver = std::variant<std::monostate, PirDriver, ContactDriver, Tmag5273ContactDriver,
                            Lis2dw12Driver, Sht40Driver, SmartModuleDriver>;

struct SlotState {
  ModuleCode code = ModuleCode::kNone;
  ModuleFault fault = ModuleFault::kNone;
  bool enabled = false;
  Driver driver;
};

// Reads the slot's ID EEPROM. kNone means nothing answered (empty slot).
ModuleCode read_module_code(SlotIo& io, std::uint8_t eeprom_addr);

}  // namespace talos::fw
