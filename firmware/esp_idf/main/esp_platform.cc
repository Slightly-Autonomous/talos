#include "esp_platform.h"

#include "board_tl_sn_001.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "nvs_flash.h"

namespace talos::fw::esp {

namespace {
const char* key_name(StoreKey k) {
  switch (k) {
    case StoreKey::kBootEpoch:
      return "boot";
    case StoreKey::kConfig:
      return "cfg";
    case StoreKey::kOta:
      return "ota";
    case StoreKey::kPendingCritical:
      return "pend";
  }
  return "x";
}
}  // namespace

TimeMs Clock::now_ms() const { return static_cast<TimeMs>(esp_timer_get_time() / 1000); }

// Busy-wait: the FreeRTOS tick (10 ms) is too coarse for datasheet delays of 1-5 ms.
void Clock::delay_ms(std::uint32_t ms) { esp_rom_delay_us(ms * 1000); }

Gpio::Gpio(const BoardSupport& b) {
  for (const auto& s : b.slots) {
    if (s.wake_line == 0xFF) continue;
    gpio_config_t io{};
    io.pin_bit_mask = 1ULL << s.wake_line;
    io.mode = GPIO_MODE_INPUT;
    // The board has 1 MOhm pull-downs on every WAKE line. The internal ~45 kOhm
    // pull-down would load a PIR output (100 uA max before false alarms) to ~75 uA.
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io);
  }
}

bool Gpio::read(std::uint8_t line) { return gpio_get_level(static_cast<gpio_num_t>(line)) != 0; }

I2c::I2c(int sda, int scl) {
  i2c_master_bus_config_t cfg{};
  cfg.i2c_port = I2C_NUM_0;
  cfg.sda_io_num = static_cast<gpio_num_t>(sda);
  cfg.scl_io_num = static_cast<gpio_num_t>(scl);
  cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  cfg.glitch_ignore_cnt = 7;
  cfg.flags.enable_internal_pullup = 1;
  i2c_new_master_bus(&cfg, &bus_);
}

i2c_master_dev_handle_t I2c::device(std::uint8_t addr) {
  if (addr >= devices_.size() || !bus_) return nullptr;
  if (!devices_[addr]) {
    i2c_device_config_t dev{};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = addr;
    dev.scl_speed_hz = 100000;
    if (i2c_master_bus_add_device(bus_, &dev, &devices_[addr]) != ESP_OK) return nullptr;
  }
  return devices_[addr];
}

bool I2c::write(std::uint8_t addr, const std::uint8_t* data, std::size_t n) {
  auto d = device(addr);
  return d && i2c_master_transmit(d, data, n, 50) == ESP_OK;
}

bool I2c::write_read(std::uint8_t addr, const std::uint8_t* w, std::size_t wn, std::uint8_t* r,
                     std::size_t rn) {
  auto d = device(addr);
  if (!d) return false;
  if (wn == 0) return i2c_master_receive(d, r, rn, 50) == ESP_OK;
  return i2c_master_transmit_receive(d, w, wn, r, rn, 50) == ESP_OK;
}

void Power::set_rail(std::uint8_t slot, Rail rail, bool on) {
  if (rail != Rail::kSwitched5v || slot >= kSlots || board::kRail5vEnable[slot] < 0) return;
  const auto pin = static_cast<gpio_num_t>(board::kRail5vEnable[slot]);
  gpio_set_direction(pin, GPIO_MODE_OUTPUT);
  gpio_set_level(pin, on ? 1 : 0);
}

std::uint32_t Power::battery_mv() { return 3900; }  // placeholder until ADC wiring exists

Store::Store() {
  if (nvs_flash_init() != ESP_OK) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  nvs_open("talos", NVS_READWRITE, &handle_);
}

std::optional<std::size_t> Store::get(StoreKey key, std::uint8_t* buf, std::size_t cap) {
  size_t len = cap;
  if (nvs_get_blob(handle_, key_name(key), buf, &len) != ESP_OK) return std::nullopt;
  return len;
}

bool Store::put(StoreKey key, const std::uint8_t* data, std::size_t n) {
  return nvs_set_blob(handle_, key_name(key), data, n) == ESP_OK && nvs_commit(handle_) == ESP_OK;
}

}  // namespace talos::fw::esp
