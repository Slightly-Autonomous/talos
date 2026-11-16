#include "firmware/sim/virtual_board.h"

#include <algorithm>
#include <cmath>

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
    if (n != 1) return false;
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

Lis2dw12::Lis2dw12(Gpio& gpio, std::uint8_t int_line) : gpio_(gpio), line_(int_line) {
  regs[0x0F] = 0x44;  // WHO_AM_I
}

void Lis2dw12::impact(std::int32_t mg) {
  const double threshold_mg = regs[0x34] * 31.25;
  const bool int1_wakeup = (regs[0x23] & 0x20) != 0;
  if (!int1_wakeup || regs[0x34] == 0 || std::abs(mg) < threshold_mg) return;
  // X axis carries the impact: 0.244 mg/LSB, 14-bit left-justified.
  const auto lsb = static_cast<std::int16_t>(std::clamp(mg * 1000 / 244, -8191, 8191) * 4);
  regs[0x28] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(lsb) & 0xFF);
  regs[0x29] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(lsb) >> 8);
  regs[0x2A] = regs[0x2B] = regs[0x2C] = regs[0x2D] = 0;
  regs[0x38] |= 0x08;  // WU_IA latched
  gpio_.set(line_, true);
}

void Lis2dw12::after_read(std::uint8_t reg) {
  if (reg == 0x38) {  // WAKE_UP_SRC clears on read and releases INT1
    regs[0x38] = static_cast<std::uint8_t>(regs[0x38] & ~0x08);
    gpio_.set(line_, false);
  }
}

// --- SHT40 ------------------------------------------------------------------------------

bool Sht40::write(const std::uint8_t* data, std::size_t n) {
  if (n != 1 || data[0] != 0xFD) return false;
  const auto t_raw =
      static_cast<std::uint16_t>(std::lround((temperature_c + 45.0) * 65535.0 / 175.0));
  const auto rh_raw =
      static_cast<std::uint16_t>(std::lround((humidity_pct + 6.0) * 65535.0 / 125.0));
  out_ = {static_cast<std::uint8_t>(t_raw >> 8),  static_cast<std::uint8_t>(t_raw & 0xFF),  0,
          static_cast<std::uint8_t>(rh_raw >> 8), static_cast<std::uint8_t>(rh_raw & 0xFF), 0};
  out_[2] = Sht40Driver::crc8(out_.data(), 2);
  out_[5] = Sht40Driver::crc8(out_.data() + 3, 2);
  if (corrupt_crc) out_[5] = static_cast<std::uint8_t>(out_[5] ^ 0xFF);
  ready_ = true;
  return true;
}

bool Sht40::read(std::uint8_t* out, std::size_t n) {
  if (!ready_ || n != 6) return false;
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

Node::Node(BoardSupport b, std::uint32_t flash_capacity)
    : i2c(b.mux_address), flash(flash_capacity), board(b), platform_{clock, gpio,  i2c,  power,
                                                                     store, flash, link, board} {}

void Node::insert(std::uint8_t slot, ModuleCode code) {
  const std::uint8_t branch = board.slots[slot].i2c_branch;
  eeproms_[slot] = std::make_unique<IdEeprom>(code, 1);
  i2c.attach(branch, board.id_eeprom_address, eeproms_[slot].get());
  if (code == ModuleCode::kLis2dw12) {
    imus_[slot] = std::make_unique<Lis2dw12>(gpio, board.slots[slot].wake_line);
    i2c.attach(branch, Lis2dw12Driver::kAddr, imus_[slot].get());
  } else if (code == ModuleCode::kSht40) {
    shts_[slot] = std::make_unique<Sht40>();
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
  for (TimeMs i = 0; i < ms; ++i) {
    clock.advance(1);
    bool edge = false;
    for (std::uint8_t l = 0; l < 32; ++l) {
      const bool v = gpio.read(l);
      if (v != last_lines_[l]) edge = true;
      last_lines_[l] = v;
    }
    if (edge) runtime_->on_interrupt();
    if (edge || clock.now_ms() >= runtime_->next_wake_ms() || !link.downlink.empty()) {
      runtime_->step();
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
