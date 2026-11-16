// ESP32-H2 entry point: the same NodeRuntime the host tests and twin run.
#include <algorithm>

#include "board_tl_sn_001.h"
#include "esp_log.h"
#include "esp_platform.h"
#include "firmware/portable/runtime.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void) {
  using namespace talos::fw;
  static const BoardSupport board = board::tl_sn_001();
  static esp::Clock clock;
  static esp::Gpio gpio(board);
  static esp::I2c i2c(board::kI2cSda, board::kI2cScl);
  static esp::Power power;
  static esp::Store store;
  static esp::Flash flash;
  static esp::Link link;
  static Platform platform{clock, gpio, i2c, power, store, flash, link, board};
  static NodeRuntime runtime(platform);

  runtime.boot();
  // Stable, parseable lines for bench checks and HIL tests (tests/hil).
  static const char* kTag = "talos";
  ESP_LOGI(kTag, "talos boot epoch=%lu version=%lu",
           static_cast<unsigned long>(runtime.boot_epoch()),
           static_cast<unsigned long>(runtime.running_version()));
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    const auto& st = runtime.slot(i);
    ESP_LOGI(kTag, "talos slot %u module=0x%04x enabled=%d fault=%d", i,
             static_cast<unsigned>(st.code), st.enabled ? 1 : 0, static_cast<int>(st.fault));
  }
  TimeMs next_log = clock.now_ms();
  for (;;) {
    runtime.step();
    if (runtime.reboot_requested()) esp_restart();
    const TimeMs now = clock.now_ms();
    if (now >= next_log) {
      ESP_LOGI(kTag, "talos alive queued=%u mode=%u", static_cast<unsigned>(runtime.queued()),
               static_cast<unsigned>(runtime.power_mode()));
      next_log = now + 10'000;
    }
    const TimeMs wake = runtime.next_wake_ms();
    // Light polling until GPIO wake interrupts and sleep are wired (Phase 6).
    const TimeMs wait = std::clamp<TimeMs>(wake > now ? wake - now : 1, 1, 20);
    vTaskDelay(pdMS_TO_TICKS(wait) > 0 ? pdMS_TO_TICKS(wait) : 1);
  }
}
