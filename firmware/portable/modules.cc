#include "firmware/portable/modules.h"

#include <algorithm>
#include <cstdlib>

namespace talos::fw {

namespace {

constexpr ModuleInfo kRegistry[] = {
    {ModuleCode::kPirEkmb, "pir_ekmb_v1", true, false, true, 1},
    {ModuleCode::kReedContact, "reed_contact_v1", true, false, true, 0},
    {ModuleCode::kTmag5273Contact, "tl_mod_001_magnetic", true, false, true, 0},
    {ModuleCode::kLis2dw12, "vibration_lis2dw12_v1", true, false, true, 1},
    {ModuleCode::kSht40, "environment_sht40_v1", false, false, false, 1},
    {ModuleCode::kSmartCameraS3, "smart_camera_s3_v1", false, true, false, 600},
};

}  // namespace

const ModuleInfo* find_module(ModuleCode code) {
  for (const auto& m : kRegistry) {
    if (m.code == code) return &m;
  }
  return nullptr;
}

// --- SlotIo -----------------------------------------------------------------------------

bool SlotIo::select_branch() {
  const auto branch = wiring().i2c_branch;
  if (branch == 0xFF) return false;
  const std::uint8_t mask = static_cast<std::uint8_t>(1u << branch);
  return p_.i2c.write(p_.board.mux_address, &mask, 1);
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

ModuleCode read_module_code(SlotIo& io, std::uint8_t eeprom_addr) {
  std::uint8_t id[3] = {};
  if (!io.reg_read(eeprom_addr, 0x00, id, 3)) return ModuleCode::kNone;
  return static_cast<ModuleCode>(static_cast<std::uint16_t>((id[0] << 8) | id[1]));
}

// --- PIR --------------------------------------------------------------------------------

bool PirDriver::init(SlotIo& io, IEventSink&, TimeMs) {
  last_ = io.read_wake_line();
  return true;
}

void PirDriver::poll(SlotIo& io, IEventSink& sink, TimeMs) {
  const bool now_level = io.read_wake_line();
  if (now_level == last_) return;
  last_ = now_level;
  sink.emit(io.slot(), EventKind::kMotion, now_level ? Priority::kP0Critical : Priority::kP1State,
            now_level, 0, 0);
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
         io.reg_write(kAddr, kIntConfig1, 0x44) &&   // threshold INT on INT pin, latched
         io.reg_write(kAddr, kDeviceConfig2, 0x23);  // W&S mode
}

bool Tmag5273ContactDriver::init(SlotIo& io, IEventSink& sink, TimeMs) {
  std::uint8_t id[2] = {};
  if (!io.reg_read(kAddr, kManufacturerIdLsb, id, 2) ||
      static_cast<std::uint16_t>((id[1] << 8) | id[0]) != kManufacturerId) {
    return false;
  }
  // One continuous-mode conversion of all axes to learn the current state.
  std::int32_t ut[3] = {};
  if (!io.reg_write(kAddr, kSensorConfig1, 0x70) || !io.reg_write(kAddr, kDeviceConfig2, 0x02) ||
      !read_field(io, ut)) {
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
  if (!read_field(io, ut)) {  // the read also clears the latch
    sink.emit(io.slot(), EventKind::kModuleFault, Priority::kP2Health, false,
              static_cast<std::int32_t>(ModuleFault::kBusError), 0);
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
  if (!arm(io, ut)) {  // never leave the sensor in standby (0.45 mA) unarmed
    sink.emit(io.slot(), EventKind::kModuleFault, Priority::kP2Health, false,
              static_cast<std::int32_t>(ModuleFault::kBusError), 0);
  }
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
  return io.reg_write(kAddr, kCtrl1, 0x50) &&  // 100 Hz, low-power mode
         io.reg_write(kAddr, kCtrl6, 0x00) &&  // +/-2 g
         set_threshold(io, threshold_mg) &&
         io.reg_write(kAddr, kCtrl4Int1, 0x20);  // wake-up event on INT1
}

void Lis2dw12Driver::poll(SlotIo& io, IEventSink& sink, TimeMs) {
  if (!io.read_wake_line()) return;
  std::uint8_t src = 0;
  if (!io.reg_read(kAddr, kWakeUpSrc, &src, 1)) {
    sink.emit(io.slot(), EventKind::kModuleFault, Priority::kP2Health, false,
              static_cast<std::int32_t>(ModuleFault::kBusError), 0);
    return;
  }
  if ((src & 0x08) == 0) return;  // WU_IA not set
  std::uint8_t raw[6] = {};
  if (!io.reg_read(kAddr, kOutXL, raw, 6)) return;
  std::int32_t peak = 0;
  for (int axis = 0; axis < 3; ++axis) {
    const auto v = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(raw[2 * axis] | (raw[2 * axis + 1] << 8)));
    // 14-bit left-justified; 0.244 mg/LSB at +/-2 g.
    std::int32_t mg = (static_cast<std::int32_t>(v) / 4) * 244 / 1000;
    if (mg < 0) mg = -mg;
    if (mg > peak) peak = mg;
  }
  sink.emit(io.slot(), EventKind::kImpact, Priority::kP0Critical, true, peak, 12);
}

// --- SHT40 ------------------------------------------------------------------------------

std::uint8_t Sht40Driver::crc8(const std::uint8_t* data, std::size_t n) {
  std::uint8_t crc = 0xFF;
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      crc = static_cast<std::uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1);
    }
  }
  return crc;
}

bool Sht40Driver::init(SlotIo&, IEventSink&, TimeMs now) {
  next_at_ = now;  // first reading right away
  return true;
}

void Sht40Driver::poll(SlotIo& io, IEventSink& sink, TimeMs now, std::uint32_t interval_ms) {
  if (now < next_at_) return;
  next_at_ = now + interval_ms;
  const std::uint8_t cmd = kMeasureHighPrecision;
  std::uint8_t r[6] = {};
  if (!io.raw_write(kAddr, &cmd, 1) || !io.raw_read(kAddr, r, 6) || crc8(r, 2) != r[2] ||
      crc8(r + 3, 2) != r[5]) {
    sink.emit(io.slot(), EventKind::kModuleFault, Priority::kP2Health, false,
              static_cast<std::int32_t>(ModuleFault::kBusError), 0);
    return;
  }
  const std::int64_t t_raw = (r[0] << 8) | r[1];
  const std::int64_t rh_raw = (r[3] << 8) | r[4];
  const auto milli_c = static_cast<std::int32_t>(-45'000 + 175'000 * t_raw / 65'535);
  std::int64_t permille = -60 + 1250 * rh_raw / 65'535;
  if (permille < 0) permille = 0;
  if (permille > 1000) permille = 1000;
  sink.emit(io.slot(), EventKind::kEnvironment, Priority::kP2Health, false, milli_c,
            static_cast<std::uint32_t>(permille));
}

}  // namespace talos::fw
