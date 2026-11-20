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
#include "firmware/portable/serial_link.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

// Diag replies and datagram frames go to stdout, which is the USB-serial-JTAG console.
class ConsoleOut final : public talos::fw::IDiagOut, public talos::fw::ILineSink {
 public:
  void write(const char* s, std::size_t n) override {
    std::fwrite(s, 1, n, stdout);
    std::fflush(stdout);
  }
};

// Collects console bytes into lines without blocking the runtime loop. Reads a
// byte at a time and stops at a newline, so a second queued line is never lost.
// Long enough for a datagram frame ("@F <base64>").
class LineReader {
 public:
  // Returns the line length (0 = no complete line yet).
  std::size_t poll() {
    std::uint8_t c = 0;
    while (usb_serial_jtag_read_bytes(&c, 1, 0) == 1) {
      if (c == '\n' || c == '\r') {
        if (len_ == 0) continue;
        const std::size_t n = overflow_ ? 0 : len_;
        line_[len_] = 0;
        len_ = 0;
        overflow_ = false;
        if (n) return n;
        continue;  // an over-long line is dropped whole, never half-parsed
      }
      if (len_ < sizeof line_ - 1) {
        line_[len_++] = static_cast<char>(c);
      } else {
        overflow_ = true;
      }
    }
    return 0;
  }
  const char* line() const { return line_; }

 private:
  char line_[talos::fw::SerialDatagram::kMaxLine + 1];
  std::size_t len_ = 0;
  bool overflow_ = false;
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
  // Longer than any blocking stretch in a step: an NVS page erase (tens of ms),
  // four slot power-ups at ~20 ms each, a full console line. PROVISIONAL.
  constexpr std::uint32_t kWatchdogTimeoutMs = 5'000;
  static esp::Watchdog watchdog(kWatchdogTimeoutMs);
  static esp::Flash flash;
  static ConsoleOut console;
  static SerialDatagram serial_net(console);
  static ProvisionedLink link(serial_net, store);  // CoAP over the console until Thread lands
  static Platform platform{clock, gpio, i2c, power, store, flash, link, board};
  static NodeRuntime runtime(platform, &watchdog);

  runtime.boot();
  // Stable, parseable lines for bench checks and HIL tests (tests/hil).
  static const char* kTag = "talos";
  ESP_LOGI(kTag, "talos boot epoch=%lu version=%lu reset=%d board_ok=%d",
           static_cast<unsigned long>(runtime.boot_epoch()),
           static_cast<unsigned long>(runtime.running_version()),
           static_cast<int>(esp_reset_reason()), runtime.board_ok() ? 1 : 0);
  ESP_LOGI(kTag, "talos persist nvs=%d epoch=%d watchdog=%d", store.ok() ? 1 : 0,
           runtime.boot_epoch_persisted() ? 1 : 0, watchdog.ok() ? 1 : 0);
  if (!runtime.boot_epoch_persisted()) {
    // Events are held until the epoch is written (retried every step).
    ESP_LOGE(kTag, "talos boot epoch not persisted: uplink held");
  }
  static Diag diag(platform, runtime, &power);
  static LineReader reader;
  ESP_LOGI(kTag, "talos link %s device=%s", link.provisioned() ? "serial" : "unprovisioned",
           link.provisioned() ? link.device_id() : "-");
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    const auto& st = runtime.slot(i);
    ESP_LOGI(kTag, "talos slot %u module=0x%04x enabled=%d fault=%d", i,
             static_cast<unsigned>(st.code), st.enabled ? 1 : 0, static_cast<int>(st.fault));
  }
  TimeMs next_log = clock.now_ms();
  for (;;) {
    if (const std::size_t n = reader.poll()) {
      if (!serial_net.push_line(reader.line(), n)) diag.handle_line(reader.line(), console);
      if (diag.reboot_requested()) esp_restart();
    }
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
