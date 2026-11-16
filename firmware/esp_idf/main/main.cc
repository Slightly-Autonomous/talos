// ESP32-H2 entry point: the same NodeRuntime the host tests and twin run.
#include <algorithm>

#include "board_proto_a.h"
#include "esp_platform.h"
#include "firmware/portable/runtime.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" void app_main(void) {
  using namespace talos::fw;
  static const BoardSupport board = board::proto_a();
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
  for (;;) {
    runtime.step();
    if (runtime.reboot_requested()) esp_restart();
    const TimeMs now = clock.now_ms();
    const TimeMs wake = runtime.next_wake_ms();
    // Light polling until GPIO wake interrupts and sleep are wired (Phase 6).
    const TimeMs wait = std::clamp<TimeMs>(wake > now ? wake - now : 1, 1, 20);
    vTaskDelay(pdMS_TO_TICKS(wait) > 0 ? pdMS_TO_TICKS(wait) : 1);
  }
}
