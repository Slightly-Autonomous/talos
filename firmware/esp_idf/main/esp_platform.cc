#include "esp_platform.h"

#include "board_tl_sn_001.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "nvs_flash.h"

namespace talos::fw::esp {

namespace {
constexpr const char* kTag = "talos";

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
    case StoreKey::kIdentity:
      return "ident";
    case StoreKey::kSlotPower:
      return "slotpwr";
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
  if (b.mux_reset_line != 0xFF) {
    // Level first, then direction: the reset line never glitches low at boot.
    gpio_set_level(static_cast<gpio_num_t>(b.mux_reset_line), 1);
    gpio_config_t io{};
    io.pin_bit_mask = 1ULL << b.mux_reset_line;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    output_ = b.mux_reset_line;
  }
  // Shared SPI (sn-mcu-slots M3): SCK and MOSI driven low with no pull, MISO an
  // input with the internal pull-down so it never floats. GPIO0-5 are digital
  // pads that do not keep this in deep sleep (H2 datasheet §2.2): the board
  // needs its own pull-downs for that, and sleep entry must re-park these first.
  for (const std::uint8_t line : {b.spi_sck_line, b.spi_mosi_line}) {
    if (line == 0xFF) continue;
    gpio_set_level(static_cast<gpio_num_t>(line), 0);  // level first: no high glitch
    gpio_config_t io{};
    io.pin_bit_mask = 1ULL << line;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io);
  }
  if (b.spi_miso_line != 0xFF) {
    gpio_config_t io{};
    io.pin_bit_mask = 1ULL << b.spi_miso_line;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&io);
  }
}

bool Gpio::read(std::uint8_t line) { return gpio_get_level(static_cast<gpio_num_t>(line)) != 0; }

bool Gpio::write(std::uint8_t line, bool level) {
  if (line != output_) return false;  // only the board's declared output may be driven
  return gpio_set_level(static_cast<gpio_num_t>(line), level ? 1 : 0) == ESP_OK;
}

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

bool I2c::probe(std::uint8_t addr) { return bus_ && i2c_master_probe(bus_, addr, 50) == ESP_OK; }

bool I2c::write_read(std::uint8_t addr, const std::uint8_t* w, std::size_t wn, std::uint8_t* r,
                     std::size_t rn) {
  auto d = device(addr);
  if (!d) return false;
  if (wn == 0) return i2c_master_receive(d, r, rn, 50) == ESP_OK;
  return i2c_master_transmit_receive(d, w, wn, r, rn, 50) == ESP_OK;
}

BatteryAdc::BatteryAdc() {
  adc_oneshot_unit_init_cfg_t unit{};
  unit.unit_id = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&unit, &unit_) != ESP_OK) {
    unit_ = nullptr;
    return;
  }
  adc_oneshot_chan_cfg_t chan{};
  chan.atten = ADC_ATTEN_DB_12;  // ~0-3.1 V at the pin: VBAT/2 tops out at 2.1 V
  chan.bitwidth = ADC_BITWIDTH_DEFAULT;
  const auto channel = static_cast<adc_channel_t>(board::kBatterySenseAdc - 1);  // GPIO1 = CH0
  if (adc_oneshot_config_channel(unit_, channel, &chan) != ESP_OK) return;
  adc_cali_curve_fitting_config_t cal{};
  cal.unit_id = ADC_UNIT_1;
  cal.chan = channel;
  cal.atten = ADC_ATTEN_DB_12;
  cal.bitwidth = ADC_BITWIDTH_DEFAULT;
  if (adc_cali_create_scheme_curve_fitting(&cal, &cali_) != ESP_OK) cali_ = nullptr;
}

std::optional<std::uint32_t> BatteryAdc::battery_mv() {
  if (!unit_ || !cali_) return std::nullopt;  // unknown, never a made-up 3.9 V
  const auto channel = static_cast<adc_channel_t>(board::kBatterySenseAdc - 1);
  std::int64_t sum = 0;
  constexpr int kSamples = 16;
  for (int i = 0; i < kSamples; ++i) {
    int raw = 0, mv = 0;
    if (adc_oneshot_read(unit_, channel, &raw) != ESP_OK ||
        adc_cali_raw_to_voltage(cali_, raw, &mv) != ESP_OK) {
      return std::nullopt;
    }
    sum += mv;
  }
  return static_cast<std::uint32_t>(sum * 2 / kSamples);  // equal divider
}

Store::Store() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // The two cases ESP-IDF documents as needing an erase. This loses the boot
    // epoch and identity: logged so the bench and the hub's epoch check can tell.
    ESP_LOGE(kTag, "talos nvs erase (%s)", esp_err_to_name(err));
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "talos nvs init failed (%s)", esp_err_to_name(err));
    return;
  }
  err = nvs_open("talos", NVS_READWRITE, &handle_);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "talos nvs open failed (%s)", esp_err_to_name(err));
    handle_ = 0;
    return;
  }
  ok_ = true;
}

std::optional<std::size_t> Store::get(StoreKey key, std::uint8_t* buf, std::size_t cap) {
  if (!ok_) return std::nullopt;
  size_t len = cap;
  if (nvs_get_blob(handle_, key_name(key), buf, &len) != ESP_OK) return std::nullopt;
  return len;
}

bool Store::put(StoreKey key, const std::uint8_t* data, std::size_t n) {
  if (!ok_) return false;
  return nvs_set_blob(handle_, key_name(key), data, n) == ESP_OK && nvs_commit(handle_) == ESP_OK;
}

Watchdog::Watchdog(std::uint32_t timeout_ms) {
  esp_task_wdt_config_t cfg{};
  cfg.timeout_ms = timeout_ms;
  cfg.idle_core_mask = 1u;   // single-core H2: keep watching IDLE on core 0 as well
  cfg.trigger_panic = true;  // panic -> reset (CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT)
  // The startup code normally has the TWDT running already (CONFIG_ESP_TASK_WDT_INIT).
  esp_err_t err = esp_task_wdt_reconfigure(&cfg);
  if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_init(&cfg);
  if (err == ESP_OK) err = esp_task_wdt_add(nullptr);  // this task (app_main)
  ok_ = err == ESP_OK;
  if (!ok_) ESP_LOGE(kTag, "talos watchdog unavailable (%s)", esp_err_to_name(err));
}

void Watchdog::feed() {
  if (ok_) esp_task_wdt_reset();
}

bool Flash::mark_running_valid() {
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) {
    return true;  // factory image or no otadata: nothing for the bootloader to roll back
  }
  if (state != ESP_OTA_IMG_PENDING_VERIFY) return true;
  return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

}  // namespace talos::fw::esp
