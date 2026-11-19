// ESP32-H2 entry point: the same NodeRuntime the host tests and twin run.
#include <algorithm>
#include <cstdio>

#include "board_tl_sn_001.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_platform.h"
#include "esp_system.h"
#include "firmware/portable/board_io.h"
#include "firmware/portable/diag.h"
#include "firmware/portable/runtime.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

// Diag replies go to stdout, which is the USB-serial-JTAG console.
class ConsoleOut final : public talos::fw::IDiagOut {
 public:
  void write(const char* s, std::size_t n) override {
    std::fwrite(s, 1, n, stdout);
    std::fflush(stdout);
  }
};

// Collects console bytes into lines without blocking the runtime loop. Reads a
// byte at a time and stops at a newline, so a second queued command is never lost.
class LineReader {
 public:
  bool poll(char* out, std::size_t cap) {
    std::uint8_t c = 0;
    while (usb_serial_jtag_read_bytes(&c, 1, 0) == 1) {
      if (c == '\n' || c == '\r') {
        if (len_ == 0) continue;
        const std::size_t m = len_ < cap - 1 ? len_ : cap - 1;
        for (std::size_t k = 0; k < m; ++k) out[k] = buf_[k];
        out[m] = 0;
        len_ = 0;
        return true;
      }
      if (len_ < sizeof buf_) buf_[len_++] = static_cast<char>(c);
    }
    return false;
  }

 private:
  char buf_[talos::fw::Diag::kMaxLine];
  std::size_t len_ = 0;
};

}  // namespace

extern "C" void app_main(void) {
  using namespace talos::fw;
  usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  usb_serial_jtag_driver_install(&usb_cfg);
  usb_serial_jtag_vfs_use_driver();
  static const BoardSupport board = board::tl_sn_001();
  static esp::Clock clock;
  static esp::Gpio gpio(board);
  static esp::I2c i2c(board::kI2cSda, board::kI2cScl);
  static esp::BatteryAdc adc;
  static BoardIo power(i2c, clock, adc, board);  // expanders, rails, source, charger
  static esp::Store store;
  static esp::Flash flash;
  static esp::Link link;
  static Platform platform{clock, gpio, i2c, power, store, flash, link, board};
  static NodeRuntime runtime(platform);

  runtime.boot();
  // Stable, parseable lines for bench checks and HIL tests (tests/hil).
  static const char* kTag = "talos";
  ESP_LOGI(kTag, "talos boot epoch=%lu version=%lu reset=%d board_ok=%d",
           static_cast<unsigned long>(runtime.boot_epoch()),
           static_cast<unsigned long>(runtime.running_version()),
           static_cast<int>(esp_reset_reason()), runtime.board_ok() ? 1 : 0);
  static Diag diag(platform, runtime, &power);
  static ConsoleOut console;
  static LineReader reader;
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    const auto& st = runtime.slot(i);
    ESP_LOGI(kTag, "talos slot %u module=0x%04x enabled=%d fault=%d", i,
             static_cast<unsigned>(st.code), st.enabled ? 1 : 0, static_cast<int>(st.fault));
  }
  TimeMs next_log = clock.now_ms();
  for (;;) {
    char line[Diag::kMaxLine];
    if (reader.poll(line, sizeof line)) diag.handle_line(line, console);
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
    const TimeMs wait =
        std::clamp<TimeMs>(wake > now ? wake - now : 1, 1, 20);  // also console latency
    vTaskDelay(pdMS_TO_TICKS(wait) > 0 ? pdMS_TO_TICKS(wait) : 1);
  }
}
