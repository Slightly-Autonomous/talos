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
#include "firmware/portable/tamper_detect.h"
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
  // TL-SN-002 built-ins (ADR 0019): soldered on, never read from an ID EEPROM.
  kTmag5273A2 = 0x0105,  // TMAG5273A2 (+/-133 mT range class): magnet-defeat headroom
  kPir223d = 0x0106,     // PIR223D-S0 digital PIR on a switched rail (PIR_PWR)
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
  // PowerProfile, or kBoardDefault = BoardSupport::power_profile (ConfigUpdate).
  static constexpr std::uint8_t kBoardDefault = 0xFF;
  std::uint8_t power_profile = kBoardDefault;
  std::uint8_t reserved[3] = {};
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
// Drivers whose state matters across an ESP32-H2 deep-sleep wake (a reset with
// the card still powered) also implement retained_state() and resume(): resume
// reads the sensor's latched event first and reports it as a transition, and
// never re-runs a power-up sequence. supervise() runs at every heartbeat.

// --- Drivers ----------------------------------------------------------------------------

// Panasonic EKMB (TL-MOD-002). Output is undefined for 25 s typ / 210 s max
// after power-up, then high while it sees motion (held ~2.6 s), then masked
// ~1.3 s. Nothing is reported until warm-up ends: the state is unknown, never
// "clear". A rise must last kGlitchMs, so an ESD or RF spike is not motion.
// Warm-up and glitch timing of one PIR part.
struct PirTiming {
  TimeMs warm_min_ms;       // nothing is valid before this after power-up
  TimeMs quiet_ms;          // ...and the output must also rest low this long
  TimeMs warm_max_ms;       // past this a still-busy output is a fault (or continuous motion)
  TimeMs glitch_ms;         // a rise shorter than this is not motion
  TimeMs toggle_window_ms;  // "still toggling" window at warm_max_ms
};

class PirDriver {
 public:
  static constexpr TimeMs kWarmMinMs = 30'000;   // datasheet Twu 25 s typ, plus margin
  static constexpr TimeMs kQuietMs = 3'000;      // output must also rest low this long
  static constexpr TimeMs kWarmMaxMs = 210'000;  // datasheet Twu max
  static constexpr TimeMs kGlitchMs = 30;
  // Past kWarmMaxMs the output is valid by the datasheet. If it still toggles
  // (low within this window: the EKMB masks ~1.3 s after every detection),
  // continuous motion is what kept it from settling, so warm-up ends and the
  // motion is reported. An output that never goes low stays a fault.
  static constexpr TimeMs kToggleWindowMs = 10'000;
  static constexpr PirTiming kEkmb{kWarmMinMs, kQuietMs, kWarmMaxMs, kGlitchMs, kToggleWindowMs};
  // PIR223D-S0 (TL-SN-002). NEWOPTO XYC-PIR223D-S0 spec (2020-06-01): output
  // held 1 s after the last valid pulse ("delay time"), then locked 2.3 s;
  // REL needs two threshold crossings within 4 s (or one at 5x threshold).
  // No warm-up time is given; factory figures are taken "after one minute
  // stabilization", so nothing is valid before 60 s, the output must rest low
  // 3 s (hold + lock), and a still-busy output is a fault at 120 s. Bench (HD-044).
  static constexpr PirTiming kPir223d{60'000, 3'000, 120'000, 30, 10'000};
  // Selects the part's timing; call before init()/resume().
  void set_timing(const PirTiming& t) { timing_ = t; }
  const PirTiming& timing() const { return timing_; }
  bool init(SlotIo& io, IEventSink& sink, TimeMs now);
  // state: 1 = warm-up was already over before the deep sleep (AON stayed on).
  bool resume(SlotIo& io, IEventSink& sink, TimeMs now, std::uint8_t state);
  std::uint8_t retained_state() const { return warming_ ? 0 : 1; }
  void poll(SlotIo& io, IEventSink& sink, TimeMs now);
  TimeMs due_ms(TimeMs now) const;
  bool warming() const { return warming_; }
  std::uint32_t glitches() const { return glitches_; }

 private:
  static constexpr TimeMs kWarmCheckMs = 250;
  PirTiming timing_ = kEkmb;
  TimeMs powered_at_ = 0;
  TimeMs next_check_ = 0;  // absolute: due_ms() must not move with the clock
  TimeMs low_since_ = 0;
  TimeMs seen_low_at_ = 0;  // last poll that saw the output low
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
  bool resume(SlotIo& io, IEventSink& sink, TimeMs now, std::uint8_t state);
  std::uint8_t retained_state() const { return stable_ ? 1 : 0; }
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
  static constexpr std::uint8_t kAddr = 0x35;  // TMAG5273A1 (contact card) factory address
  static constexpr std::uint8_t kDeviceConfig2 = 0x01, kSensorConfig1 = 0x02;
  static constexpr std::uint8_t kSensorConfig2 = 0x03, kXThr = 0x04, kIntConfig1 = 0x08;
  static constexpr std::uint8_t kManufacturerIdLsb = 0x0E, kXMsbResult = 0x12;
  static constexpr std::uint8_t kConvStatus = 0x18;         // bit 0 RESULT_STATUS
  static constexpr std::uint16_t kManufacturerId = 0x5449;  // "TI"
  static constexpr std::uint8_t kSleep100Ms = 0x07;
  static constexpr std::uint8_t kModeSleep = 0x01, kModeContinuous = 0x02, kModeWakeSleep = 0x03;
  // Door counts as open below 10 mT and closed above 20 mT (hysteresis). With the reference
  // 20 x 5 x 3 mm N35 magnet: closed within ~10 mm, open beyond ~14 mm (magnet_sim.py, S-5).
  // 3 / 1.5 mT read closed up to ~24 mm. Provisional until bench C1-06 measures the real door.
  static constexpr std::int32_t kOpenUt = 10'000;
  static constexpr std::int32_t kCloseUt = 20'000;
  static constexpr std::int32_t kLsbPerMt = 820;  // +/-40 mT range (A1)
  // TL-SN-002's built-in Hall sensor: the x2 range class, +/-133 mT (or +/-266 mT
  // with X_Y_RANGE/Z_RANGE set). A threshold code is 133 mT / 128. The letter is the
  // address variant (SLYS045C Table 6-2: A2 35h, B2 22h). Fitted part: TMAG5273B2
  // at 0x22 since 2026-11-24 (the A2 went out of stock at JLCPCB; HD-045). The
  // ModuleCode and module id keep the "A2" name: they name the range class.
  static constexpr std::uint8_t kX2Addr = 0x22;
  static constexpr std::int32_t kA2RangeMt = 133;
  static constexpr std::int32_t kA2LsbPerMt = 250;  // SLYS045C 5.8: 250 LSB/mT at +/-133 mT
  // Range variant; call before init()/resume(). Defaults to the A1 (+/-40 mT).
  void set_range(std::int32_t range_mt, std::int32_t lsb_per_mt) {
    range_mt_ = range_mt;
    lsb_per_mt_ = lsb_per_mt;
  }
  std::int32_t range_mt() const { return range_mt_; }
  // I2C address variant; call before init()/resume(). Defaults to the A1's 0x35.
  void set_address(std::uint8_t addr) { addr_ = addr; }
  std::uint8_t address() const { return addr_; }
  // A failed convert/re-arm (an I2C glitch) is retried: kRetryMs apart up to
  // kMaxRetries, then the sensor is parked in sleep (5 nA, not 0.45 mA standby),
  // reported (kBusError, b = kFaultDetailUnarmed) and retried every kSlowRetryMs.
  static constexpr TimeMs kRetryMs = 50;
  static constexpr int kMaxRetries = 10;
  static constexpr TimeMs kSlowRetryMs = 60'000;
  bool init(SlotIo& io, IEventSink& sink, TimeMs now);
  // state: 1 = the door was open before the deep sleep.
  bool resume(SlotIo& io, IEventSink& sink, TimeMs now, std::uint8_t state);
  std::uint8_t retained_state() const { return open_ ? 1 : 0; }
  void poll(SlotIo& io, IEventSink& sink, TimeMs now);
  // Heartbeat check: a sensor that left wake-and-sleep while WAKE stayed low
  // latched an event the MCU never saw (WAKE stuck low or open): report it.
  void supervise(SlotIo& io, IEventSink& sink, TimeMs now);
  TimeMs due_ms(TimeMs) const { return retry_at_; }
  // 8-bit threshold code: range / 128 per code (40 mT for the A1), rounded up.
  static std::uint8_t threshold_code(std::int32_t ut, std::int32_t range_mt = 40);
  bool open() const { return open_; }
  // The last fresh 3-axis conversion (uT) and a counter that moves with each one,
  // for the runtime's magnet-defeat check (tamper_detect.h).
  const Vec3& field() const { return field_; }
  std::uint32_t field_seq() const { return field_seq_; }

 private:
  bool convert_all(SlotIo& io, std::int32_t (&ut)[3]);
  bool read_field(SlotIo& io, std::int32_t (&ut)[3]);
  bool arm(SlotIo& io, const std::int32_t (&ut)[3]);
  // Convert, report a change, re-arm; on failure schedule a bounded retry.
  void service(SlotIo& io, IEventSink& sink, TimeMs now);
  void failed(SlotIo& io, IEventSink& sink, TimeMs now);
  bool open_ = true;
  Vec3 field_{};
  std::uint32_t field_seq_ = 0;
  std::int32_t range_mt_ = 40;
  std::int32_t lsb_per_mt_ = kLsbPerMt;
  std::uint8_t addr_ = kAddr;
  TimeMs retry_at_ = kNever;
  int failures_ = 0;
  bool unarmed_reported_ = false;
  bool wake_fault_reported_ = false;
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
  // Deep-sleep wake: read the latched wake-up source before anything else
  // (no soft reset), then check the configuration and re-init only if it is lost.
  bool resume(SlotIo& io, IEventSink& sink, TimeMs now, std::uint32_t threshold_mg);
  void poll(SlotIo& io, IEventSink& sink, TimeMs now);
  // Heartbeat check: a latched wake-up with WAKE low means the line is stuck
  // low or open; report the fault and the impact.
  void supervise(SlotIo& io, IEventSink& sink, TimeMs now);
  bool set_threshold(SlotIo& io, std::uint32_t threshold_mg);
  // Gravity in mg (TamperCause::kMountMoved): the outputs normally carry the
  // high-pass path (FDS), so this switches them to the low-pass path for
  // kGravitySettleMs, reads one sample and switches back. The wake-up function
  // keeps running on its own filter meanwhile. PROVISIONAL (bench HD-049).
  static constexpr std::uint32_t kGravitySettleMs = 25;  // > 2 ODR periods at 100 Hz
  bool read_gravity(SlotIo& io, Vec3& mg);
  // A wake line held high produces no new edge, so re-check it until it is
  // either explained by an event or reported as stuck.
  TimeMs due_ms(TimeMs) const {
    return stuck_reads_ > 0 && !stuck_reported_ ? recheck_at_ : kNever;
  }
  static std::uint8_t threshold_register(std::uint32_t threshold_mg);

 private:
  static constexpr TimeMs kRecheckMs = 100;
  bool configured(SlotIo& io, std::uint32_t threshold_mg);
  bool report_impact(SlotIo& io, IEventSink& sink);
  TimeMs recheck_at_ = 0;
  bool wake_fault_reported_ = false;
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
