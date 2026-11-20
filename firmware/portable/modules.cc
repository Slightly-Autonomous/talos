#include "firmware/portable/modules.h"

#include <algorithm>
#include <cstdlib>

namespace talos::fw {

namespace {

constexpr ModuleInfo kRegistry[] = {
    {ModuleCode::kPirEkmb, "pir_ekmb_v1", true, false, true, 1},
    {ModuleCode::kReedContact, "reed_contact_v1", true, false, true, 0},
    {ModuleCode::kTmag5273Contact, "tl_mod_001_magnetic", true, false, true, 0},
    {ModuleCode::kTlMod002Pir, "tl_mod_002_pir", true, false, true, 0},
    {ModuleCode::kLis2dw12, "vibration_lis2dw12_v1", true, false, true, 1},
    {ModuleCode::kTlMod003Imu, "tl_mod_003_imu", true, false, true, 0},
    {ModuleCode::kSht40, "environment_sht40_v1", false, false, false, 1},
    {ModuleCode::kTlMod004Env, "tl_mod_004_environment", false, false, false, 0},
    {ModuleCode::kSmartCameraS3, "smart_camera_s3_v1", false, true, false, 600},
    {ModuleCode::kTlTest001, "tl_test_001", false, false, false, 0},
};

}  // namespace

const ModuleInfo* find_module(ModuleCode code) {
  for (const auto& m : kRegistry) {
    if (m.code == code) return &m;
  }
  return nullptr;
}

std::uint8_t crc8(const std::uint8_t* data, std::size_t n) {
  std::uint8_t crc = 0xFF;
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      crc = static_cast<std::uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1);
    }
  }
  return crc;
}

std::array<std::uint8_t, kIdBlockSize> make_id_block(ModuleCode code, std::uint8_t revision,
                                                     std::uint8_t variant) {
  std::array<std::uint8_t, kIdBlockSize> b{};
  b.fill(0xFF);
  const auto c = static_cast<std::uint16_t>(code);
  b[0] = static_cast<std::uint8_t>(c >> 8);
  b[1] = static_cast<std::uint8_t>(c & 0xFF);
  b[2] = revision;
  b[3] = kIdFormatV1;
  b[4] = variant;
  b[15] = crc8(b.data(), 15);
  return b;
}

// --- SlotIo -----------------------------------------------------------------------------

namespace {

// TCA9546A RESET_N low clears the control register (every branch off), which
// frees a root bus that a card holds stuck through its branch. 1 ms is far
// beyond the 6 ns minimum pulse.
bool reset_mux(Platform& p) {
  const std::uint8_t line = p.board.mux_reset_line;
  if (line == 0xFF || !p.gpio.write(line, false)) return false;
  p.clock.delay_ms(1);
  p.gpio.write(line, true);
  return true;
}

}  // namespace

bool SlotIo::select_branch() {
  const auto branch = wiring().i2c_branch;
  if (branch == 0xFF) return false;
  const std::uint8_t mask = static_cast<std::uint8_t>(1u << branch);
  selected_ = true;  // even on failure: the mux may have latched it
  if (p_.i2c.write(p_.board.mux_address, &mask, 1)) return true;
  // The root bus may be held by the branch selected last: reset the mux and try once more.
  return reset_mux(p_) && p_.i2c.write(p_.board.mux_address, &mask, 1);
}

bool SlotIo::deselect() {
  if (!selected_) return true;
  selected_ = false;
  const std::uint8_t none = 0;
  if (p_.i2c.write(p_.board.mux_address, &none, 1)) return true;
  // A card holding SDA low blocks the deselect itself; the reset line still works.
  return reset_mux(p_);
}

bool SlotIo::read_wake_line() {
  return wiring().wake_line != 0xFF && p_.gpio.read(wiring().wake_line);
}

bool SlotIo::reg_read(std::uint8_t addr, std::uint8_t reg, std::uint8_t* out, std::size_t n) {
  return select_branch() && p_.i2c.write_read(addr, &reg, 1, out, n);
}

bool SlotIo::reg_write(std::uint8_t addr, std::uint8_t reg, std::uint8_t value) {
  const std::uint8_t buf[2] = {reg, value};
  return select_branch() && p_.i2c.write(addr, buf, 2);
}

bool SlotIo::raw_write(std::uint8_t addr, const std::uint8_t* data, std::size_t n) {
  return select_branch() && p_.i2c.write(addr, data, n);
}

bool SlotIo::raw_read(std::uint8_t addr, std::uint8_t* out, std::size_t n) {
  return select_branch() && p_.i2c.write_read(addr, nullptr, 0, out, n);
}

IdRead read_module_id(SlotIo& io, std::uint8_t eeprom_addr) {
  IdRead r;
  if (!io.select_branch()) {
    r.status = IdRead::Status::kMuxFault;
    return r;
  }
  std::uint8_t id[kIdBlockSize] = {};
  if (!io.reg_read(eeprom_addr, 0x00, id, kIdBlockSize)) return r;  // nothing answered: empty
  bool all_ff = true;
  for (auto b : id) all_ff &= b == 0xFF;
  const auto code = static_cast<std::uint16_t>((id[0] << 8) | id[1]);
  r.revision = id[2];
  r.variant = id[4];
  if (all_ff) {
    r.status = IdRead::Status::kBlank;
  } else if (id[3] != kIdFormatV1 || crc8(id, kIdBlockSize - 1) != id[kIdBlockSize - 1] ||
             code == 0x0000 || code == 0xFFFF) {
    r.status = IdRead::Status::kCorrupt;
  } else {
    r.status = IdRead::Status::kOk;
    r.code = static_cast<ModuleCode>(code);
  }
  return r;
}

namespace {
void bus_fault(SlotIo& io, IEventSink& sink, std::uint32_t detail = 0) {
  sink.emit(io.slot(), EventKind::kModuleFault, Priority::kP2Health, false,
            static_cast<std::int32_t>(ModuleFault::kBusError), detail);
}
}  // namespace

// --- PIR --------------------------------------------------------------------------------

bool PirDriver::init(SlotIo&, IEventSink&, TimeMs now) {
  powered_at_ = now;  // the slot's AON rail came up with discovery
  next_check_ = now;
  low_since_ = now;
  warming_ = true;
  timeout_reported_ = false;
  reported_ = false;
  high_since_ = kNever;
  return true;
}

void PirDriver::poll(SlotIo& io, IEventSink& sink, TimeMs now) {
  const bool high = io.read_wake_line();
  if (warming_) {
    next_check_ = now + kWarmCheckMs;
    if (high) low_since_ = now;
    if (now - powered_at_ >= kWarmMinMs && !high && now - low_since_ >= kQuietMs) {
      warming_ = false;
    } else if (now - powered_at_ >= kWarmMaxMs && !timeout_reported_) {
      timeout_reported_ = true;
      sink.emit(io.slot(), EventKind::kModuleFault, Priority::kP2Health, false,
                static_cast<std::int32_t>(ModuleFault::kWarmupTimeout), 0);
    }
    return;
  }
  if (high && !reported_) {
    if (high_since_ == kNever) high_since_ = now;
    if (now - high_since_ >= kGlitchMs) {
      reported_ = true;
      sink.emit(io.slot(), EventKind::kMotion, Priority::kP0Critical, true, 0, 0);
    }
  } else if (!high) {
    if (high_since_ != kNever && !reported_) ++glitches_;  // fell before kGlitchMs
    high_since_ = kNever;
    if (reported_) {
      reported_ = false;
      sink.emit(io.slot(), EventKind::kMotion, Priority::kP1State, false, 0, 0);
    }
  }
}

TimeMs PirDriver::due_ms(TimeMs) const {
  if (warming_) return next_check_;  // check the settle condition a few times a second
  if (high_since_ != kNever && !reported_) return high_since_ + kGlitchMs;
  return kNever;
}

// --- Contact (line high = open) ---------------------------------------------------------

bool ContactDriver::init(SlotIo& io, IEventSink& sink, TimeMs now) {
  stable_ = candidate_ = io.read_wake_line();
  since_ = now;
  // Boot report of the current state (b = 1): not a transition, so it must
  // never look like a new opening to rules.
  sink.emit(io.slot(), EventKind::kContact, Priority::kP1State, stable_, 0, 1);
  return true;
}

void ContactDriver::poll(SlotIo& io, IEventSink& sink, TimeMs now) {
  const bool raw = io.read_wake_line();
  if (raw != candidate_) {
    candidate_ = raw;
    since_ = now;
  }
  if (candidate_ != stable_ && now - since_ >= kDebounceMs) {
    stable_ = candidate_;
    sink.emit(io.slot(), EventKind::kContact, stable_ ? Priority::kP0Critical : Priority::kP1State,
              stable_, 0, 0);
  }
}

// --- TMAG5273 contact -------------------------------------------------------------------

std::uint8_t Tmag5273ContactDriver::threshold_code(std::int32_t ut) {
  std::int32_t code = (ut * 128 + 39'999) / 40'000;
  if (code < 1) code = 1;
  if (code > 127) code = 127;
  return static_cast<std::uint8_t>(code);
}

bool Tmag5273ContactDriver::read_field(SlotIo& io, std::int32_t (&ut)[3]) {
  std::uint8_t raw[6] = {};
  if (!io.reg_read(kAddr, kXMsbResult, raw, 6)) return false;
  for (int axis = 0; axis < 3; ++axis) {
    const auto v = static_cast<std::int16_t>(
        static_cast<std::uint16_t>((raw[2 * axis] << 8) | raw[2 * axis + 1]));
    ut[axis] = static_cast<std::int32_t>(v) * 1000 / kLsbPerMt;
  }
  return true;
}

// One continuous-mode conversion of all three axes. A conversion takes
// ~0.2 ms (tstart 70 us + 3 channels); wait 1 ms, then insist RESULT_STATUS
// says the results are new before trusting them.
bool Tmag5273ContactDriver::convert_all(SlotIo& io, std::int32_t (&ut)[3]) {
  if (!io.reg_write(kAddr, kSensorConfig1, 0x70) ||
      !io.reg_write(kAddr, kDeviceConfig2, kModeContinuous)) {
    return false;
  }
  for (int attempt = 0; attempt < 3; ++attempt) {
    io.delay_ms(1);
    std::uint8_t status = 0;
    if (!io.reg_read(kAddr, kConvStatus, &status, 1)) return false;
    if (status & 0x01) return read_field(io, ut);
  }
  return false;
}

bool Tmag5273ContactDriver::arm(SlotIo& io, const std::int32_t (&ut)[3]) {
  int dominant = 0;
  for (int axis = 1; axis < 3; ++axis) {
    if (std::abs(ut[axis]) > std::abs(ut[dominant])) dominant = axis;
  }
  // Standby first so the new thresholds apply cleanly, then symmetric (+/-)
  // thresholds (THR_HYST = 1). A zero threshold disables that axis' check.
  if (!io.reg_write(kAddr, kDeviceConfig2, 0x20)) return false;
  const std::uint8_t channels = open_ ? 0x7 : static_cast<std::uint8_t>(1u << dominant);
  for (int axis = 0; axis < 3; ++axis) {
    const bool checked = open_ || axis == dominant;
    const std::uint8_t code = checked ? threshold_code(open_ ? kCloseUt : kOpenUt) : 0;
    if (!io.reg_write(kAddr, static_cast<std::uint8_t>(kXThr + axis), code)) return false;
  }
  return io.reg_write(kAddr, kSensorConfig2, open_ ? 0x00 : 0x20) &&  // MAG_THR_DIR: above/below
         io.reg_write(kAddr, kSensorConfig1,
                      static_cast<std::uint8_t>((channels << 4) | kSleep100Ms)) &&
         io.reg_write(kAddr, kIntConfig1, 0x44) &&  // threshold INT on INT pin, latched
         io.reg_write(kAddr, kDeviceConfig2, static_cast<std::uint8_t>(0x20 | kModeWakeSleep));
}

bool Tmag5273ContactDriver::init(SlotIo& io, IEventSink& sink, TimeMs) {
  std::uint8_t id[2] = {};
  if (!io.reg_read(kAddr, kManufacturerIdLsb, id, 2) ||
      static_cast<std::uint16_t>((id[1] << 8) | id[0]) != kManufacturerId) {
    return false;
  }
  std::int32_t ut[3] = {};
  if (!convert_all(io, ut)) {
    // It answered but will not convert: park it in sleep (5 nA) rather than
    // leave it in standby (0.45 mA) for the life of the battery.
    io.reg_write(kAddr, kDeviceConfig2, kModeSleep);
    return false;
  }
  std::int32_t peak = 0;
  for (auto v : ut) peak = std::max(peak, std::abs(v));
  open_ = peak < kCloseUt;
  sink.emit(io.slot(), EventKind::kContact, Priority::kP1State, open_, peak, 1);  // state report
  return arm(io, ut);
}

void Tmag5273ContactDriver::poll(SlotIo& io, IEventSink& sink, TimeMs) {
  if (!io.read_wake_line()) return;  // WAKE high = latched threshold interrupt
  std::int32_t ut[3] = {};
  if (!convert_all(io, ut)) {  // the first access also clears the latch
    bus_fault(io, sink);
    return;
  }
  std::int32_t peak = 0;
  for (auto v : ut) peak = std::max(peak, std::abs(v));
  const bool now_open = open_ ? peak <= kCloseUt : peak < kOpenUt;
  if (now_open != open_) {
    open_ = now_open;
    sink.emit(io.slot(), EventKind::kContact, open_ ? Priority::kP0Critical : Priority::kP1State,
              open_, peak, 0);
  }
  if (!arm(io, ut)) bus_fault(io, sink);  // never leave the sensor in standby (0.45 mA) unarmed
}

// --- LIS2DW12 ---------------------------------------------------------------------------

std::uint8_t Lis2dw12Driver::threshold_register(std::uint32_t threshold_mg) {
  // +/-2 g full scale: 1 LSB of WAKE_UP_THS = FS/64 = 31.25 mg; 6-bit field.
  std::uint32_t lsb = (threshold_mg * 100 + 3124) / 3125;
  if (lsb < 1) lsb = 1;
  if (lsb > 63) lsb = 63;
  return static_cast<std::uint8_t>(lsb);
}

bool Lis2dw12Driver::set_threshold(SlotIo& io, std::uint32_t threshold_mg) {
  return io.reg_write(kAddr, kWakeUpThs, threshold_register(threshold_mg));
}

bool Lis2dw12Driver::init(SlotIo& io, IEventSink&, TimeMs, std::uint32_t threshold_mg) {
  std::uint8_t who = 0;
  if (!io.reg_read(kAddr, kWhoAmI, &who, 1) || who != kWhoAmIValue) return false;
  // AON stays up across an MCU reset, so the part may hold an old config and a
  // stale latched event: soft reset, then configure from scratch.
  if (!io.reg_write(kAddr, kCtrl2, kSoftReset)) return false;
  io.delay_ms(5);
  std::uint8_t ctrl2 = 0;
  if (!io.reg_read(kAddr, kCtrl2, &ctrl2, 1) || (ctrl2 & kSoftReset)) return false;
  struct {
    std::uint8_t reg, value;
  } const writes[] = {
      {kCtrl2, kCtrl2BduIncr},
      {kCtrl1, kCtrl1Lp100Hz},
      {kCtrl6, kCtrl6HpFds},
      {kWakeUpDur, 0x00},
      {kWakeUpThs, threshold_register(threshold_mg)},
      {kCtrl3, kCtrl3Lir},
      {kCtrl4Int1, kCtrl4Wu},
      {kCtrl7, kCtrl7IntEnable},
  };
  for (const auto& w : writes) {
    if (!io.reg_write(kAddr, w.reg, w.value)) return false;
  }
  for (const auto& w : writes) {  // a write the part did not take means a bad joint or part
    std::uint8_t got = 0;
    if (!io.reg_read(kAddr, w.reg, &got, 1) || got != w.value) return false;
  }
  std::uint8_t stale = 0;
  return io.reg_read(kAddr, kAllIntSrc, &stale, 1);  // clears anything latched before reset
}

void Lis2dw12Driver::poll(SlotIo& io, IEventSink& sink, TimeMs now) {
  if (!io.read_wake_line()) {
    stuck_reads_ = 0;
    return;
  }
  std::uint8_t src = 0;
  if (!io.reg_read(kAddr, kWakeUpSrc, &src, 1)) {
    bus_fault(io, sink);
    return;
  }
  if ((src & kWuIa) == 0) {
    // WAKE high with no event behind it: INT1 bridged high or the line stuck.
    recheck_at_ = now + kRecheckMs;
    if (++stuck_reads_ >= kStuckReads && !stuck_reported_) {
      stuck_reported_ = true;
      sink.emit(io.slot(), EventKind::kModuleFault, Priority::kP2Health, false,
                static_cast<std::int32_t>(ModuleFault::kStuckInterrupt), 0);
    }
    return;
  }
  stuck_reads_ = 0;
  std::uint8_t raw[6] = {};
  if (!io.reg_read(kAddr, kOutXL, raw, 6)) {
    bus_fault(io, sink);
    return;
  }
  std::int32_t peak = 0;
  for (int axis = 0; axis < 3; ++axis) {
    const auto v = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(raw[2 * axis] | (raw[2 * axis + 1] << 8)));
    // 14-bit left-justified; 0.244 mg/LSB at +/-2 g.
    std::int32_t mg = (static_cast<std::int32_t>(v) / 4) * 244 / 1000;
    if (mg < 0) mg = -mg;
    if (mg > peak) peak = mg;
  }
  // b (duration) is 0 = not measured: one post-event sample cannot give it.
  sink.emit(io.slot(), EventKind::kImpact, Priority::kP0Critical, true, peak, 0);
}

// --- TL-TEST-001 -----------------------------------------------------------------------

bool TestCardDriver::init(SlotIo& io, IEventSink&, TimeMs) {
  // Every expander pin an input: the card drives nothing until a test asks it to
  // (MISO is shared between slots and must stay released).
  return io.reg_write(kExpanderAddr, 0x06, 0xFF) && io.reg_write(kExpanderAddr, 0x07, 0xFF);
}

// --- SHT40 ------------------------------------------------------------------------------

bool Sht40Driver::init(SlotIo& io, IEventSink&, TimeMs now) {
  // Probe: soft reset (tSR <= 1 ms), then the serial number with both CRCs.
  const std::uint8_t reset = kSoftReset, serial = kSerial;
  if (!io.raw_write(kAddr, &reset, 1)) return false;
  io.delay_ms(2);
  if (!io.raw_write(kAddr, &serial, 1)) return false;
  io.delay_ms(2);
  std::uint8_t r[6] = {};
  if (!io.raw_read(kAddr, r, 6) || crc8(r, 2) != r[2] || crc8(r + 3, 2) != r[5]) return false;
  serial_ =
      (std::uint32_t{r[0]} << 24) | (std::uint32_t{r[1]} << 16) | (std::uint32_t{r[3]} << 8) | r[4];
  next_at_ = now;  // first reading right away
  measuring_ = false;
  return true;
}

void Sht40Driver::fail(SlotIo& io, IEventSink& sink, Fault f) {
  measuring_ = false;
  bus_fault(io, sink, static_cast<std::uint32_t>(f));
}

void Sht40Driver::poll(SlotIo& io, IEventSink& sink, TimeMs now, std::uint32_t interval_ms) {
  if (!measuring_) {
    if (now < next_at_) return;
    started_at_ = now;
    next_at_ = now + interval_ms;
    const std::uint8_t cmd = kMeasureHighPrecision;
    if (!io.raw_write(kAddr, &cmd, 1)) return fail(io, sink, Fault::kWrite);
    measuring_ = true;
    ready_at_ = now + kMeasureMs;
    return;
  }
  if (now < ready_at_) return;
  std::uint8_t r[6] = {};
  if (!io.raw_read(kAddr, r, 6)) {
    if (now - started_at_ < kMeasureMs + kResultGraceMs) {
      ready_at_ = now + 1;  // still converting: NACK on the read header
      return;
    }
    return fail(io, sink, Fault::kNoResult);
  }
  measuring_ = false;
  if (crc8(r, 2) != r[2]) return fail(io, sink, Fault::kTempCrc);
  if (crc8(r + 3, 2) != r[5]) return fail(io, sink, Fault::kRhCrc);
  const std::int64_t t_raw = (r[0] << 8) | r[1];
  const std::int64_t rh_raw = (r[3] << 8) | r[4];
  // All-zero or all-one words are what a stuck or shorted output looks like.
  if (t_raw == 0 || t_raw == 0xFFFF || rh_raw == 0 || rh_raw == 0xFFFF) {
    return fail(io, sink, Fault::kRange);
  }
  const auto milli_c = static_cast<std::int32_t>(-45'000 + 175'000 * t_raw / 65'535);
  std::int64_t permille = -60 + 1250 * rh_raw / 65'535;
  if (permille < 0) permille = 0;
  if (permille > 1000) permille = 1000;
  sink.emit(io.slot(), EventKind::kEnvironment, Priority::kP2Health, false, milli_c,
            static_cast<std::uint32_t>(permille));
}

}  // namespace talos::fw
