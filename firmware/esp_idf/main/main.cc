// ESP32-H2 entry point: the same NodeRuntime the host tests and twin run.
#include <algorithm>
#include <cstdio>

#include "board_select.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_platform.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "firmware/portable/board_io.h"
#include "firmware/portable/diag.h"
#include "firmware/portable/runtime.h"
#include "firmware/portable/serial_link.h"
#include "firmware/portable/thread_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "thread_link.h"

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
      last_rx_ms_ = esp_timer_get_time() / 1000;
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
  // A host typed on the console within the last kActiveMs: keep the node awake.
  bool active() const {
    return last_rx_ms_ >= 0 && esp_timer_get_time() / 1000 - last_rx_ms_ < kActiveMs;
  }

 private:
  static constexpr std::int64_t kActiveMs = 30'000;
  std::int64_t last_rx_ms_ = -1;
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
  static const BoardSupport board = selected_board::make();
  static esp::Clock clock;
  static esp::Gpio gpio(board);
  static esp::I2c i2c(selected_board::kI2cSda, selected_board::kI2cScl);
  static esp::BatteryAdc adc;
  static BoardIo power(i2c, clock, adc, board, &gpio);  // expanders, rails, lamps, source
  static esp::Store store;
  // Longer than any blocking stretch in a step: an NVS page erase (tens of ms),
  // four slot power-ups at ~20 ms each, a full console line. PROVISIONAL.
  constexpr std::uint32_t kWatchdogTimeoutMs = 5'000;
  static esp::Watchdog watchdog(kWatchdogTimeoutMs);
  static esp::Flash flash;
  static ConsoleOut console;
  static SerialDatagram serial_net(console);
  // CoAP over Thread once a dataset is provisioned (diag `thread`), else over the
  // USB console (bench). The same identity and wire format either way.
  static esp::ThreadDatagram thread_net;
  const auto thread_cfg = load_thread_config(store);
  const bool on_thread = thread_cfg && thread_net.start(*thread_cfg, board.power_profile);
  static ProvisionedLink link(
      on_thread ? static_cast<IDatagram&>(thread_net) : static_cast<IDatagram&>(serial_net), store);
  static Platform platform{clock, gpio, i2c, power, store, flash, link, board};
  static esp::Sleep sleeper;  // reset cause and LP RAM: a deep-sleep wake keeps the cards running
  static NodeRuntime runtime(platform, &watchdog, &sleeper);

  runtime.boot();
  // Stable, parseable lines for bench checks and HIL tests (tests/hil).
  static const char* kTag = "talos";
  ESP_LOGI(kTag, "talos target=%s", selected_board::kBoardName);
  ESP_LOGI(kTag, "talos boot epoch=%lu version=%lu reset=%d board_ok=%d",
           static_cast<unsigned long>(runtime.boot_epoch()),
           static_cast<unsigned long>(runtime.running_version()),
           static_cast<int>(esp_reset_reason()), runtime.board_ok() ? 1 : 0);
  ESP_LOGI(kTag, "talos persist nvs=%d id_nvs=%d epoch=%d watchdog=%d", store.ok() ? 1 : 0,
           store.permanent_ok() ? 1 : 0, runtime.boot_epoch_persisted() ? 1 : 0,
           watchdog.ok() ? 1 : 0);
  ESP_LOGI(kTag, "talos wake cause=%u lines=0x%08lx resumed=%d",
           static_cast<unsigned>(sleeper.wake_info().cause),
           static_cast<unsigned long>(sleeper.wake_info().lines),
           runtime.resumed_from_sleep() ? 1 : 0);
  {
    const auto info = load_board_info(store);  // D-09: written by talos_dbg program-id --node
    const auto tamper = runtime.tamper_open();
    ESP_LOGI(kTag, "talos board=%s rev=%s serial=%s tamper=%d", info ? info->board : "-",
             info ? info->revision : "-", info ? info->serial : "-",
             tamper ? (*tamper ? 1 : 0) : -1);
  }
  if (!runtime.boot_epoch_persisted()) {
    // Events are held until the epoch is written (retried every step).
    ESP_LOGE(kTag, "talos boot epoch not persisted: uplink held");
  }
  static Diag diag(platform, runtime, &power);
  static LineReader reader;
  ESP_LOGI(kTag, "talos link %s device=%s",
           !link.provisioned() ? "unprovisioned" : (on_thread ? "thread" : "serial"),
           link.provisioned() ? link.device_id() : "-");
  for (std::uint8_t i = 0; i < kSlots; ++i) {
    const auto& st = runtime.slot(i);
    ESP_LOGI(kTag, "talos slot %u module=0x%04x enabled=%d fault=%d", i,
             static_cast<unsigned>(st.code), st.enabled ? 1 : 0, static_cast<int>(st.fault));
  }
  TimeMs next_log = clock.now_ms();
  // Automatic light sleep (esp_pm) only on battery: the USB console stops in light
  // sleep, and on USB the node runs always-on anyway (sleep_policy.h).
  bool pm_light = false;
  auto set_light_sleep = [&](bool on) {
    if (on == pm_light) return;
    esp_pm_config_t pm = {};
    pm.max_freq_mhz = 96;
    pm.min_freq_mhz = 32;
    pm.light_sleep_enable = on;
    if (esp_pm_configure(&pm) == ESP_OK) pm_light = on;
  };
  PowerProfile profile_applied = board.power_profile;
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
    if (on_thread && runtime.power_profile() != profile_applied) {
      profile_applied = runtime.power_profile();  // the hub changed it (ConfigUpdate)
      thread_net.set_profile(profile_applied);
    }
    const SleepPlan plan = runtime.sleep_plan(reader.active());
    const TimeMs wake = runtime.next_wake_ms();
    TimeMs wait = 1;
    if (plan.kind == SleepPlan::Kind::kDeep) {
      // Everything is acknowledged and nothing is due soon: deep sleep until the
      // next timer or a wake line. A reset on wake; LP RAM carries the state.
      ESP_LOGI(kTag, "talos sleep deep ms=%llu",
               static_cast<unsigned long long>(plan.until_ms > now ? plan.until_ms - now : 0));
      std::fflush(stdout);
      sleeper.enter_deep(plan.until_ms > now ? plan.until_ms - now : 1);
    } else if (plan.kind == SleepPlan::Kind::kLight) {
      // Light sleep between steps; the armed wake lines end it at once. Capped
      // well inside the task watchdog (kWatchdogTimeoutMs).
      sleeper.arm_light_sleep_wake();
      set_light_sleep(true);
      wait = std::clamp<TimeMs>(wake > now ? wake - now : 1, 1, 2'000);
    } else {
      set_light_sleep(false);
      wait = std::clamp<TimeMs>(wake > now ? wake - now : 1, 1, 20);  // also console latency
    }
    vTaskDelay(pdMS_TO_TICKS(wait) > 0 ? pdMS_TO_TICKS(wait) : 1);
  }
}
