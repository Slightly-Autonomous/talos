// ESP-IDF implementations of the portable HAL (skeleton). Clock, GPIO, I2C
// and NVS are real; battery sensing, the OTA flash binding and the Thread
// link land with hardware bring-up and Phase 6.
#pragma once

#include <array>

#include "driver/i2c_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "firmware/portable/hal.h"
#include "nvs.h"

namespace talos::fw::esp {

class Clock final : public IClock {
 public:
  TimeMs now_ms() const override;
  void delay_ms(std::uint32_t ms) override;
};

class Gpio final : public IGpio {
 public:
  explicit Gpio(const BoardSupport& b);
  bool read(std::uint8_t line) override;
  bool write(std::uint8_t line, bool level) override;

 private:
  std::uint8_t output_ = 0xFF;  // the only output line (mux reset)
};

class I2c final : public II2c {
 public:
  I2c(int sda, int scl);
  bool write(std::uint8_t addr, const std::uint8_t* data, std::size_t n) override;
  bool write_read(std::uint8_t addr, const std::uint8_t* w, std::size_t wn, std::uint8_t* r,
                  std::size_t rn) override;
  bool probe(std::uint8_t addr) override;

 private:
  i2c_master_dev_handle_t device(std::uint8_t addr);
  i2c_master_bus_handle_t bus_ = nullptr;
  std::array<i2c_master_dev_handle_t, 128> devices_{};
};

// VBAT through the 4.7M/4.7M divider into ADC1 (board::kBatterySenseAdc). The
// 2.35 MOhm source impedance is smoothed by C4 (100 nF); average a burst.
class BatteryAdc final : public IBatteryAdc {
 public:
  BatteryAdc();
  std::optional<std::uint32_t> battery_mv() override;

 private:
  adc_oneshot_unit_handle_t unit_ = nullptr;
  adc_cali_handle_t cali_ = nullptr;
};

class Store final : public IStore {
 public:
  Store();
  std::optional<std::size_t> get(StoreKey key, std::uint8_t* buf, std::size_t cap) override;
  bool put(StoreKey key, const std::uint8_t* data, std::size_t n) override;

 private:
  nvs_handle_t handle_ = 0;
};

// Not wired to esp_ota_* yet: offers are refused, so OTA cannot half-apply.
class Flash final : public IFlash {
 public:
  std::uint32_t slot_capacity() const override { return 0; }
  bool erase(std::uint8_t) override { return false; }
  bool write(std::uint8_t, std::uint32_t, const std::uint8_t*, std::size_t) override {
    return false;
  }
  bool read(std::uint8_t, std::uint32_t, std::uint8_t*, std::size_t) override { return false; }
};

}  // namespace talos::fw::esp
