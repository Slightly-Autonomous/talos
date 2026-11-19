// Module registry, slot discovery and drivers (spec §31-32).
//
// Every module carries an ID EEPROM on its slot's I2C branch (2-byte module
// code + revision). Discovery reads it and picks a driver from the static
// registry; the board's SlotWiring decides whether the slot can host it.
// Register maps follow the datasheets as understood today and are
// PROVISIONAL until validated on hardware (ADR 0013).
#pragma once

#include <array>
#include <cstdint>
#include <variant>

#include "firmware/portable/hal.h"
#include "firmware/portable/types.h"

namespace talos::fw {

// Module codes. 0x01xx contact/motion, 0x02xx vibration, 0x03xx environment,
// 0x04xx intelligent modules, 0xF0xx bench fixtures. The 0x0101/0x0201/0x0301
// codes are the dev-kit modules; the Rev A cards have their own codes so each
// maps to its own manifest (contracts/manifests/modules/*.yaml).
enum class ModuleCode : std::uint16_t {
  kNone = 0,
  kPirEkmb = 0x0101,
  kReedContact = 0x0102,
  kTmag5273Contact = 0x0103,  // TL-MOD-001: 3-axis Hall, orientation-independent
  kTlMod002Pir = 0x0104,      // TL-MOD-002: EKMB PIR card
  kLis2dw12 = 0x0201,
  kTlMod003Imu = 0x0202,  // TL-MOD-003: LIS2DW12 card
  kSht40 = 0x0301,
  kTlMod004Env = 0x0302,  // TL-MOD-004: SHT40 card
  kSmartCameraS3 = 0x0401,
  kTlTest001 = 0xF001,  // TL-TEST-001 slot test card (bench fixture)
};

// CRC-8, polynomial 0x31, init 0xFF (Sensirion's, also used for the ID block).
std::uint8_t crc8(const std::uint8_t* data, std::size_t n);

// Card ID block v1: page 0 (16 bytes) of the 24AA025E48.
//   0-1 module code (big-endian), 2 hardware revision, 3 format (0x01),
//   4 variant, 5-14 reserved (0xFF), 15 CRC-8 over bytes 0-14.
// The EUI-48 at 0xFA-0xFF is the card's serial number.
inline constexpr std::uint8_t kIdFormatV1 = 0x01;
inline constexpr std::size_t kIdBlockSize = 16;
std::array<std::uint8_t, kIdBlockSize> make_id_block(ModuleCode code, std::uint8_t revision,
                                                     std::uint8_t variant = 0);

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
// Callers deselect() when done with the slot, so a fault on one branch can
// never hold the root bus that the expanders and charger share.
class SlotIo {
 public:
  SlotIo(Platform& p, std::uint8_t slot) : p_(p), slot_(slot) {}
  std::uint8_t slot() const { return slot_; }
  const SlotWiring& wiring() const { return p_.board.slots[slot_]; }
  bool select_branch();
  // Writes the mux back to "no branch", only if this SlotIo selected one.
  bool deselect();
  bool read_wake_line();
  void delay_ms(std::uint32_t ms) { p_.clock.delay_ms(ms); }
  TimeMs now() const { return p_.clock.now_ms(); }
  bool reg_read(std::uint8_t addr, std::uint8_t reg, std::uint8_t* out, std::size_t n);
  bool reg_write(std::uint8_t addr, std::uint8_t reg, std::uint8_t value);
  bool raw_write(std::uint8_t addr, const std::uint8_t* data, std::size_t n);
  bool raw_read(std::uint8_t addr, std::uint8_t* out, std::size_t n);

 private:
  Platform& p_;
  std::uint8_t slot_;
  bool selected_ = false;
};

inline constexpr TimeMs kNever = ~TimeMs{0};

// Every driver implements init(), poll() and due_ms(): the earliest time it
// needs poll() again without an interrupt (kNever when only a wake line will do).

// --- Drivers ----------------------------------------------------------------------------

// Panasonic EKMB (TL-MOD-002). Output is undefined for 25 s typ / 210 s max
// after power-up, then high while it sees motion (held ~2.6 s), then masked
// ~1.3 s. Nothing is reported until warm-up ends: the state is unknown, never
// "clear". A rise must last kGlitchMs, so an ESD or RF spike is not motion.
class PirDriver {
 public:
  static constexpr TimeMs kWarmMinMs = 30'000;   // datasheet Twu 25 s typ, plus margin
  static constexpr TimeMs kQuietMs = 3'000;      // output must also rest low this long
  static constexpr TimeMs kWarmMaxMs = 210'000;  // datasheet Twu max
  static constexpr TimeMs kGlitchMs = 30;
  bool init(SlotIo& io, IEventSink& sink, TimeMs now);
  void poll(SlotIo& io, IEventSink& sink, TimeMs now);
  TimeMs due_ms(TimeMs now) const;
  bool warming() const { return warming_; }
  std::uint32_t glitches() const { return glitches_; }

 private:
  static constexpr TimeMs kWarmCheckMs = 250;
  TimeMs powered_at_ = 0;
  TimeMs next_check_ = 0;  // absolute: due_ms() must not move with the clock
  TimeMs low_since_ = 0;
  TimeMs high_since_ = kNever;
  bool warming_ = true;
  bool timeout_reported_ = false;
  bool reported_ = false;
  std::uint32_t glitches_ = 0;
};

// Reed/Hall contact with time-based debounce: a level must hold for
// kDebounceMs before it becomes the reported state (spec §3 bounce example).
class ContactDriver {
 public:
  static constexpr TimeMs kDebounceMs = 20;
  bool init(SlotIo& io, IEventSink& sink, TimeMs now);
  void poll(SlotIo& io, IEventSink& sink, TimeMs now);
  TimeMs due_ms(TimeMs now) const { return candidate_ != stable_ ? now : kNever; }

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
// so the magnet can sit at any angle. The state is always decided from a fresh
// conversion of all three axes: in wake-and-sleep only the armed axes convert,
// so the other result registers are stale. Config is volatile: init() runs
// after every power-up.
class Tmag5273ContactDriver {
 public:
  static constexpr std::uint8_t kAddr = 0x35;  // TMAG5273A1 factory address
  static constexpr std::uint8_t kDeviceConfig2 = 0x01, kSensorConfig1 = 0x02;
  static constexpr std::uint8_t kSensorConfig2 = 0x03, kXThr = 0x04, kIntConfig1 = 0x08;
  static constexpr std::uint8_t kManufacturerIdLsb = 0x0E, kXMsbResult = 0x12;
  static constexpr std::uint8_t kConvStatus = 0x18;         // bit 0 RESULT_STATUS
  static constexpr std::uint16_t kManufacturerId = 0x5449;  // "TI"
  static constexpr std::uint8_t kSleep100Ms = 0x07;
  static constexpr std::uint8_t kModeSleep = 0x01, kModeContinuous = 0x02, kModeWakeSleep = 0x03;
  static constexpr std::int32_t kOpenUt = 1'500;   // door counts as open below 1.5 mT
  static constexpr std::int32_t kCloseUt = 3'000;  // and closed above 3 mT (hysteresis)
  static constexpr std::int32_t kLsbPerMt = 820;   // +/-40 mT range
  bool init(SlotIo& io, IEventSink& sink, TimeMs now);
  void poll(SlotIo& io, IEventSink& sink, TimeMs now);
  TimeMs due_ms(TimeMs) const { return kNever; }
  // 8-bit threshold code for +/-40 mT range: 40 mT / 128 per code, rounded up.
  static std::uint8_t threshold_code(std::int32_t ut);
  bool open() const { return open_; }

 private:
  bool convert_all(SlotIo& io, std::int32_t (&ut)[3]);
  bool read_field(SlotIo& io, std::int32_t (&ut)[3]);
  bool arm(SlotIo& io, const std::int32_t (&ut)[3]);
  bool open_ = true;
};

// LIS2DW12 (TL-MOD-003, ST DS11811). Interrupts are latched (LIR) and routed
// with INTERRUPTS_ENABLE, so an impact survives until the driver reads it even
// when the loop polls slower than one ODR period. Outputs come from the
// high-pass path (FDS), so the reported peak excludes gravity.
class Lis2dw12Driver {
 public:
  static constexpr std::uint8_t kAddr = 0x19;
  static constexpr std::uint8_t kWhoAmI = 0x0F, kWhoAmIValue = 0x44;
  static constexpr std::uint8_t kCtrl1 = 0x20, kCtrl2 = 0x21, kCtrl3 = 0x22, kCtrl4Int1 = 0x23;
  static constexpr std::uint8_t kCtrl6 = 0x25, kOutXL = 0x28, kWakeUpDur = 0x35;
  static constexpr std::uint8_t kWakeUpThs = 0x34, kWakeUpSrc = 0x38, kAllIntSrc = 0x3B;
  static constexpr std::uint8_t kCtrl7 = 0x3F;
  static constexpr std::uint8_t kCtrl1Lp100Hz = 0x50, kCtrl2BduIncr = 0x0C, kSoftReset = 0x40;
  static constexpr std::uint8_t kCtrl3Lir = 0x10, kCtrl4Wu = 0x20, kCtrl6HpFds = 0x08;
  static constexpr std::uint8_t kCtrl7IntEnable = 0x20;
  static constexpr std::uint8_t kWuIa = 0x08;
  static constexpr int kStuckReads = 3;
  bool init(SlotIo& io, IEventSink& sink, TimeMs now, std::uint32_t threshold_mg);
  void poll(SlotIo& io, IEventSink& sink, TimeMs now);
  bool set_threshold(SlotIo& io, std::uint32_t threshold_mg);
  // A wake line held high produces no new edge, so re-check it until it is
  // either explained by an event or reported as stuck.
  TimeMs due_ms(TimeMs) const {
    return stuck_reads_ > 0 && !stuck_reported_ ? recheck_at_ : kNever;
  }
  static std::uint8_t threshold_register(std::uint32_t threshold_mg);

 private:
  static constexpr TimeMs kRecheckMs = 100;
  TimeMs recheck_at_ = 0;
  int stuck_reads_ = 0;
  bool stuck_reported_ = false;
};

// SHT40 (TL-MOD-004, Sensirion SHT4x). The sensor NACKs its read header while
// it measures (up to 8.3 ms at high precision), so a measurement is two steps:
// send the command, then read once due_ms() says the result is ready.
// Fault detail in event b: 1 write NACK, 2 no result, 3 T CRC, 4 RH CRC,
// 5 implausible raw value.
class Sht40Driver {
 public:
  static constexpr std::uint8_t kAddr = 0x44;
  static constexpr std::uint8_t kMeasureHighPrecision = 0xFD, kSoftReset = 0x94, kSerial = 0x89;
  static constexpr TimeMs kMeasureMs = 9;       // 8.3 ms max, plus margin
  static constexpr TimeMs kResultGraceMs = 20;  // keep retrying the read this long
  enum class Fault : std::uint32_t {
    kWrite = 1,
    kNoResult = 2,
    kTempCrc = 3,
    kRhCrc = 4,
    kRange = 5
  };
  bool init(SlotIo& io, IEventSink& sink, TimeMs now);
  void poll(SlotIo& io, IEventSink& sink, TimeMs now, std::uint32_t interval_ms);
  TimeMs due_ms(TimeMs) const { return measuring_ ? ready_at_ : next_at_; }
  std::uint32_t serial() const { return serial_; }
  static std::uint8_t crc8(const std::uint8_t* data, std::size_t n) { return fw::crc8(data, n); }

 private:
  void fail(SlotIo& io, IEventSink& sink, Fault f);
  TimeMs next_at_ = 0;
  TimeMs ready_at_ = 0;
  TimeMs started_at_ = 0;
  bool measuring_ = false;
  std::uint32_t serial_ = 0;
};

// TL-TEST-001 slot test card: a TCA9535 at 0x27 on the slot branch that drives
// and reads the slot's lines. In normal running it only proves it answers; the
// bench self-test (diag) drives it.
class TestCardDriver {
 public:
  static constexpr std::uint8_t kExpanderAddr = 0x27;
  bool init(SlotIo& io, IEventSink& sink, TimeMs now);
  void poll(SlotIo&, IEventSink&, TimeMs) {}
  TimeMs due_ms(TimeMs) const { return kNever; }
};

// Intelligent module (camera): presence and power only; it has its own
// processor, radio and OTA (spec §33).
class SmartModuleDriver {
 public:
  bool init(SlotIo&, IEventSink&, TimeMs) { return true; }
  void poll(SlotIo&, IEventSink&, TimeMs) {}
  TimeMs due_ms(TimeMs) const { return kNever; }
};

using Driver = std::variant<std::monostate, PirDriver, ContactDriver, Tmag5273ContactDriver,
                            Lis2dw12Driver, Sht40Driver, SmartModuleDriver, TestCardDriver>;

struct SlotState {
  ModuleCode code = ModuleCode::kNone;
  ModuleFault fault = ModuleFault::kNone;
  bool enabled = false;
  Driver driver;
};

// Result of reading a slot's ID EEPROM. An empty slot and a slot whose mux
// branch cannot be selected are different things, and so are a blank and a
// corrupt ID: each gets its own fault instead of looking like "empty".
struct IdRead {
  enum class Status : std::uint8_t { kAbsent, kOk, kBlank, kCorrupt, kMuxFault };
  Status status = Status::kAbsent;
  ModuleCode code = ModuleCode::kNone;
  std::uint8_t revision = 0;
  std::uint8_t variant = 0;
};

IdRead read_module_id(SlotIo& io, std::uint8_t eeprom_addr);

}  // namespace talos::fw
