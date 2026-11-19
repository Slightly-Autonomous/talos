#include "firmware/sim/virtual_board.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace talos::fw::sim {

// --- I2C --------------------------------------------------------------------------------

I2cDevice* I2cBus::target(std::uint8_t addr) {
  int branch = -1;
  for (int b = 0; b < 8; ++b) {
    if (mask_ & (1u << b)) {
      if (branch != -1) return nullptr;  // two branches: address clash risk; refuse
      branch = b;
    }
  }
  if (branch < 0 || broken_[static_cast<std::size_t>(branch)]) return nullptr;
  auto it = devices_.find({static_cast<std::uint8_t>(branch), addr});
  return it == devices_.end() ? nullptr : it->second;
}

bool I2cBus::write(std::uint8_t addr, const std::uint8_t* data, std::size_t n) {
  ++transactions_;
  if (addr == mux_) {
    if (n != 1 || mux_nack) return false;
    mask_ = data[0];
    return true;
  }
  I2cDevice* d = target(addr);
  return d && d->write(data, n);
}

bool I2cBus::write_read(std::uint8_t addr, const std::uint8_t* w, std::size_t wn, std::uint8_t* r,
                        std::size_t rn) {
  ++transactions_;
  I2cDevice* d = target(addr);
  if (!d) return false;
  if (wn > 0 && !d->write(w, wn)) return false;
  return d->read(r, rn);
}

bool RegisterDevice::write(const std::uint8_t* data, std::size_t n) {
  if (n == 0) return false;
  ptr_ = data[0];
  for (std::size_t i = 1; i < n; ++i) regs[ptr_++] = data[i];
  return true;
}

bool RegisterDevice::read(std::uint8_t* out, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = regs[ptr_];
    after_read(ptr_);
    ++ptr_;
  }
  return true;
}

IdEeprom::IdEeprom(ModuleCode code, std::uint8_t revision) {
  const auto c = static_cast<std::uint16_t>(code);
  regs[0] = static_cast<std::uint8_t>(c >> 8);
  regs[1] = static_cast<std::uint8_t>(c & 0xFF);
  regs[2] = revision;
}

Lis2dw12::Lis2dw12(Gpio& gpio, std::uint8_t int_line, const Clock& clock)
    : gpio_(gpio), line_(int_line), clock_(clock) {
  reset_registers();
}

void Lis2dw12::reset_registers() {
  regs.fill(0);
  regs[0x0F] = 0x44;  // WHO_AM_I
  regs[0x21] = 0x04;  // CTRL2 reset value: IF_ADD_INC
  pulsing_ = false;
}

void Lis2dw12::set_int(bool v) { gpio_.set(line_, v || int_stuck_high); }

bool Lis2dw12::write(const std::uint8_t* data, std::size_t n) {
  const std::uint8_t keep = read_only_reg >= 0 ? regs[static_cast<std::size_t>(read_only_reg)] : 0;
  const bool ok = RegisterDevice::write(data, n);
  if (read_only_reg >= 0) regs[static_cast<std::size_t>(read_only_reg)] = keep;
  if (regs[0x21] & 0x40) {  // CTRL2 SOFT_RESET: registers back to defaults, bit self-clears
    reset_registers();
    set_int(false);
  }
  return ok;
}

void Lis2dw12::impact(std::int32_t mg) {
  const double threshold_mg = (regs[0x34] & 0x3F) * 31.25;
  const bool int1_wakeup = (regs[0x23] & 0x20) != 0;
  const bool interrupts_enabled = (regs[0x3F] & 0x20) != 0;
  if ((regs[0x34] & 0x3F) == 0 || std::abs(mg) < threshold_mg) return;
  // X carries the impact; Z carries gravity unless the high-pass path is selected.
  const bool high_pass = (regs[0x25] & 0x08) != 0;
  auto put = [&](std::uint8_t reg, std::int32_t axis_mg) {
    const auto lsb = static_cast<std::int16_t>(std::clamp(axis_mg * 1000 / 244, -8191, 8191) * 4);
    regs[reg] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(lsb) & 0xFF);
    regs[reg + 1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(lsb) >> 8);
  };
  put(0x28, mg);
  put(0x2A, 0);
  put(0x2C, high_pass ? 0 : 1000);
  regs[0x38] |= 0x08;  // WU_IA
  regs[0x3B] |= 0x02;  // ALL_INT_SRC WU_IA
  if (!int1_wakeup || !interrupts_enabled) return;
  set_int(true);
  // Without LIR the pin (and WU_IA) last one ODR period: 100 Hz -> 10 ms.
  const std::uint8_t odr = regs[0x20] >> 4;
  const TimeMs period = odr >= 6 ? 5 : odr == 5 ? 10 : odr == 4 ? 20 : 80;
  pulsing_ = (regs[0x22] & 0x10) == 0;
  pulse_until_ = clock_.now_ms() + period;
}

void Lis2dw12::tick() {
  if (pulsing_ && clock_.now_ms() >= pulse_until_) {
    pulsing_ = false;
    regs[0x38] = static_cast<std::uint8_t>(regs[0x38] & ~0x08);
    regs[0x3B] = static_cast<std::uint8_t>(regs[0x3B] & ~0x02);
    set_int(false);
  }
}

void Lis2dw12::after_read(std::uint8_t reg) {
  if (reg == 0x38 || reg == 0x3B) {  // reading either source register clears the latch
    regs[0x38] = static_cast<std::uint8_t>(regs[0x38] & ~0x08);
    regs[0x3B] = static_cast<std::uint8_t>(regs[0x3B] & ~0x02);
    pulsing_ = false;
    set_int(false);
  }
}

// --- TMAG5273 ---------------------------------------------------------------------------

Tmag5273::Tmag5273(Gpio& gpio, std::uint8_t wake_line, const Clock& clock)
    : gpio_(gpio), line_(wake_line), clock_(clock) {
  regs[0x0D] = 0x01;  // DEVICE_ID: +/-40/80 mT version
  regs[0x0E] = 0x49;  // MANUFACTURER_ID "TI"
  regs[0x0F] = 0x54;
}

void Tmag5273::set_field(std::int32_t x_ut, std::int32_t y_ut, std::int32_t z_ut) {
  field_ut_ = {x_ut, y_ut, z_ut};
  evaluate();  // a W&S wake converts the armed channels; nothing else updates
}

void Tmag5273::update_results() {
  const std::uint8_t channels = regs[0x02] >> 4;  // MAG_CH_EN: bit 0 X, 1 Y, 2 Z
  for (int axis = 0; axis < 3; ++axis) {
    if ((channels & (1u << axis)) == 0) continue;
    const auto lsb = static_cast<std::int16_t>(
        std::clamp(field_ut_[axis] * Tmag5273ContactDriver::kLsbPerMt / 1000, -32768, 32767));
    regs[0x12 + 2 * axis] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(lsb) >> 8);
    regs[0x13 + 2 * axis] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(lsb) & 0xFF);
  }
}

void Tmag5273::evaluate() {
  // Threshold interrupt only in W&S mode with THRSLD_INT and INT through the INT pin.
  if (!in_wake_sleep() || (regs[0x08] & 0x40) == 0 || ((regs[0x08] >> 2) & 0x7) != 1) return;
  update_results();
  const bool below = (regs[0x03] & 0x20) != 0;
  bool fire = false;
  for (int axis = 0; axis < 3; ++axis) {
    const std::uint8_t code = regs[0x04 + axis] & 0x7F;  // THR_HYST = 1: 7 LSBs, +/- band
    if (code == 0) continue;
    const std::int32_t thr_ut = code * 40'000 / 128;
    const std::int32_t b = std::abs(field_ut_[axis]);
    fire |= below ? b < thr_ut : b > thr_ut;
  }
  if (!fire) return;
  regs[0x01] = static_cast<std::uint8_t>(regs[0x01] & ~0x03);  // exits W&S to standby
  gpio_.set(line_, true);                                      // INT latched (WAKE high)
}

bool Tmag5273::write(const std::uint8_t* data, std::size_t n) {
  gpio_.set(line_, false);  // any valid address clears the latched INT
  const std::uint8_t mode_before = regs[0x01] & 0x03;
  const bool ok = RegisterDevice::write(data, n);
  const std::uint8_t mode = regs[0x01] & 0x03;
  if (mode != mode_before) {
    regs[0x18] = static_cast<std::uint8_t>(regs[0x18] & ~0x01);  // RESULT_STATUS: no new data yet
    continuous_since_ = clock_.now_ms();
  }
  evaluate();  // newly armed W&S converts on its first wake
  return ok;
}

// Continuous mode: the first conversion of the enabled channels is ready
// about 0.2 ms after the mode change; the model rounds that up to 1 ms.
void Tmag5273::settle_conversion() {
  if (!never_converts && (regs[0x01] & 0x03) == 0x02 && clock_.now_ms() >= continuous_since_ + 1) {
    update_results();
    regs[0x18] |= 0x01;
  }
}

bool Tmag5273::read(std::uint8_t* out, std::size_t n) {
  gpio_.set(line_, false);
  settle_conversion();
  return RegisterDevice::read(out, n);
}

// --- SHT40 ------------------------------------------------------------------------------

void Sht40::load(std::uint16_t a, std::uint16_t b) {
  out_ = {static_cast<std::uint8_t>(a >> 8), static_cast<std::uint8_t>(a & 0xFF), 0,
          static_cast<std::uint8_t>(b >> 8), static_cast<std::uint8_t>(b & 0xFF), 0};
  out_[2] = Sht40Driver::crc8(out_.data(), 2);
  out_[5] = Sht40Driver::crc8(out_.data() + 3, 2);
}

bool Sht40::write(const std::uint8_t* data, std::size_t n) {
  const TimeMs now = clock_.now_ms();
  if (n != 1 || now < busy_until_) return false;  // NACK while a command runs
  ready_ = false;
  switch (data[0]) {
    case 0xFD: {
      const auto t_raw =
          static_cast<std::uint16_t>(std::lround((temperature_c + 45.0) * 65535.0 / 175.0));
      const auto rh_raw =
          static_cast<std::uint16_t>(std::lround((humidity_pct + 6.0) * 65535.0 / 125.0));
      load(t_raw, rh_raw);
      if (corrupt_t_crc) out_[2] = static_cast<std::uint8_t>(out_[2] ^ 0xFF);
      if (corrupt_crc) out_[5] = static_cast<std::uint8_t>(out_[5] ^ 0xFF);
      busy_until_ = now + measure_ms;
      ready_ = true;
      return true;
    }
    case 0x89:
      load(static_cast<std::uint16_t>(serial >> 16), static_cast<std::uint16_t>(serial & 0xFFFF));
      busy_until_ = now + 1;
      ready_ = true;
      return true;
    case 0x94:
      busy_until_ = now + 1;
      return true;
    default:
      return false;
  }
}

bool Sht40::read(std::uint8_t* out, std::size_t n) {
  if (!ready_ || n != 6 || clock_.now_ms() < busy_until_) return false;
  std::copy(out_.begin(), out_.end(), out);
  ready_ = false;
  return true;
}

// --- Store / Flash / Link -----------------------------------------------------------------

std::optional<std::size_t> Store::get(StoreKey key, std::uint8_t* buf, std::size_t cap) {
  auto it = kv_.find(key);
  if (it == kv_.end() || it->second.size() > cap) return std::nullopt;
  std::copy(it->second.begin(), it->second.end(), buf);
  return it->second.size();
}

bool Store::put(StoreKey key, const std::uint8_t* data, std::size_t n) {
  ++puts;
  if (fail_puts > 0) {
    --fail_puts;
    return false;  // power lost before the write committed: old value stays
  }
  kv_[key] = std::vector<std::uint8_t>(data, data + n);
  return true;
}

bool Flash::erase(std::uint8_t slot) {
  if (slot > 1) return false;
  std::fill(slots_[slot].begin(), slots_[slot].end(), 0xFF);
  return true;
}

bool Flash::write(std::uint8_t slot, std::uint32_t offset, const std::uint8_t* data,
                  std::size_t n) {
  if (slot > 1 || offset + n > slots_[slot].size()) return false;
  std::copy(data, data + n, slots_[slot].begin() + offset);
  return true;
}

bool Flash::read(std::uint8_t slot, std::uint32_t offset, std::uint8_t* out, std::size_t n) {
  if (slot > 1 || offset + n > slots_[slot].size()) return false;
  std::copy(slots_[slot].begin() + offset,
            slots_[slot].begin() + offset + static_cast<std::ptrdiff_t>(n), out);
  return true;
}

bool Link::send(const DeviceEvent& e) {
  ++attempts;
  if (!connected) return false;
  if (drop_next > 0) {
    --drop_next;
    return true;  // the radio took it; it was lost in the air
  }
  uplink.push_back(e);
  return true;
}

std::optional<Downlink> Link::receive() {
  if (!connected || downlink.empty()) return std::nullopt;
  Downlink d = downlink.front();
  downlink.pop_front();
  return d;
}

// --- Node -------------------------------------------------------------------------------

BoardSupport default_board() {
  BoardSupport b;
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    b.slots[i] = SlotWiring{true, i, i, i == 2};
  }
  return b;
}

Node::Node(BoardSupport b, std::uint32_t flash_capacity, ILink* link_override)
    : i2c(b.mux_address),
      flash(flash_capacity),
      board(b),
      platform_{clock, gpio, i2c, power, store, flash, link_override ? *link_override : link,
                board} {}

void Node::insert(std::uint8_t slot, ModuleCode code) {
  const std::uint8_t branch = board.slots[slot].i2c_branch;
  eeproms_[slot] = std::make_unique<IdEeprom>(code, 1);
  i2c.attach(branch, board.id_eeprom_address, eeproms_[slot].get());
  if (code == ModuleCode::kLis2dw12) {
    imus_[slot] = std::make_unique<Lis2dw12>(gpio, board.slots[slot].wake_line, clock);
    i2c.attach(branch, Lis2dw12Driver::kAddr, imus_[slot].get());
  } else if (code == ModuleCode::kTmag5273Contact) {
    halls_[slot] = std::make_unique<Tmag5273>(gpio, board.slots[slot].wake_line, clock);
    i2c.attach(branch, Tmag5273ContactDriver::kAddr, halls_[slot].get());
  } else if (code == ModuleCode::kSht40) {
    shts_[slot] = std::make_unique<Sht40>(clock);
    i2c.attach(branch, Sht40Driver::kAddr, shts_[slot].get());
  }
}

void Node::boot() {
  runtime_.reset();
  runtime_.emplace(platform_);
  runtime_->boot();
  for (std::uint8_t l = 0; l < 32; ++l) last_lines_[l] = gpio.read(l);
}

void Node::run_for(TimeMs ms) {
  const TimeMs end = clock.now_ms() + ms;
  while (clock.now_ms() < end) {
    clock.advance(1);
    for (auto& [slot, imu] : imus_) imu->tick();
    bool edge = false;
    for (std::uint8_t l = 0; l < 32; ++l) {
      const bool v = gpio.read(l);
      if (v != last_lines_[l]) edge = true;
      last_lines_[l] = v;
    }
    bool run = false;
    if (esp_polling) {
      run = clock.now_ms() >= next_poll_;
    } else {
      if (edge) runtime_->on_interrupt();
      run =
          edge || clock.now_ms() >= runtime_->next_wake_ms() || !link.downlink.empty() || poll_link;
    }
    if (run) {
      runtime_->step();
      if (esp_polling) {
        const TimeMs now = clock.now_ms();
        const TimeMs wake = runtime_->next_wake_ms();
        next_poll_ = now + std::clamp<TimeMs>(wake > now ? wake - now : 1, 1, 20);
      }
    }
    if (runtime_->reboot_requested()) {
      ++reboots_;
      boot();
    }
  }
}

std::vector<DeviceEvent> Node::drain_and_ack() {
  // Like the hub: retransmissions of the same (boot_epoch, seq) count once.
  std::vector<DeviceEvent> out;
  for (const auto& e : link.uplink) {
    const bool seen = std::any_of(out.begin(), out.end(), [&](const DeviceEvent& o) {
      return o.boot_epoch == e.boot_epoch && o.seq == e.seq;
    });
    if (!seen) out.push_back(e);
    link.downlink.push_back(Ack{e.boot_epoch, e.seq});
  }
  link.uplink.clear();
  return out;
}

}  // namespace talos::fw::sim
