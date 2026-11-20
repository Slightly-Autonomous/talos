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
  // Also parks the board's shared SPI lines (BoardSupport::spi_*): nothing
  // drives SPI yet, and after reset GPIO4 (MTCK) has its weak pull-up on and
  // MISO floats, which would idle SCK high into every seated card.
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

// NVS on two partitions (partitions.csv). The working partition ("nvs") is
// erased only when ESP-IDF says it must be (ESP_ERR_NVS_NO_FREE_PAGES /
// ESP_ERR_NVS_NEW_VERSION_FOUND); any other init error leaves it alone and its
// get/put fail. The boot epoch and identity (is_permanent()) live in
// "talos_id", which firmware never erases: an erase of the working partition
// can't restart the epoch below what the hub remembers (red team RT-06) or
// unprovision the node. If "talos_id" can't be opened those keys fail loudly
// (the uplink is held) rather than fall back to the erasable partition.
class Store final : public IStore {
 public:
  static constexpr const char* kPermanentPartition = "talos_id";
  Store();
  std::optional<std::size_t> get(StoreKey key, std::uint8_t* buf, std::size_t cap) override;
  bool put(StoreKey key, const std::uint8_t* data, std::size_t n) override;
  bool ok() const { return ok_; }
  bool permanent_ok() const { return perm_ok_; }

 private:
  nvs_handle_t handle_ = 0;
  nvs_handle_t perm_ = 0;
  bool ok_ = false;
  bool perm_ok_ = false;
};

// Reset cause, ext1 wake lines and LP RAM for NodeRuntime (deep-sleep wakes are
// resets on the ESP32-H2). Sleep entry itself is not wired yet: main.cc still
// light-polls. The ext1 mask is kept armed so entering deep sleep needs no
// further setup. Per-pin wake levels on the H2 are unverified (HD-004).
class Sleep final : public ISleep {
 public:
  Sleep();
  WakeInfo wake_info() override { return info_; }
  bool set_wake_enabled(std::uint8_t line, bool enabled) override;
  std::uint8_t* retained() override;
  std::size_t retained_size() const override;

 private:
  WakeInfo info_;
  std::uint64_t mask_ = 0;
};

// Task watchdog with a panic (and so a reset) on timeout, subscribed for the
// task that constructs it (app_main). The runtime feeds it every step.
class Watchdog final : public IWatchdog {
 public:
  explicit Watchdog(std::uint32_t timeout_ms);
  void feed() override;
  bool ok() const { return ok_; }

 private:
  bool ok_ = false;
};

// Not wired to esp_ota_* yet: offers are refused, so OTA cannot half-apply.
// mark_running_valid() is real: if the bootloader holds the running image in
// ESP_OTA_IMG_PENDING_VERIFY, it is marked valid only when the runtime's
// health check passes (NodeRuntime::handle -> Ota::confirm).
class Flash final : public IFlash {
 public:
  bool mark_running_valid() override;
  std::uint32_t slot_capacity() const override { return 0; }
  bool erase(std::uint8_t) override { return false; }
  bool write(std::uint8_t, std::uint32_t, const std::uint8_t*, std::size_t) override {
    return false;
  }
  bool read(std::uint8_t, std::uint32_t, std::uint8_t*, std::size_t) override { return false; }
};

}  // namespace talos::fw::esp
