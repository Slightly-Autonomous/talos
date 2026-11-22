#include "thread_link.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "openthread/dataset.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/message.h"
#include "openthread/platform/radio.h"
#include "openthread/thread.h"
#include "openthread/udp.h"

namespace talos::fw::esp {

namespace {

constexpr const char* kTag = "talos.thread";
constexpr std::uint16_t kLocalPort = 5683;

otUdpSocket g_socket;

void on_receive(void* ctx, otMessage* msg, const otMessageInfo* /*info*/) {
  auto* self = static_cast<ThreadDatagram*>(ctx);
  std::uint8_t buf[ThreadDatagram::kMaxDatagram];
  const std::uint16_t off = otMessageGetOffset(msg);
  const std::uint16_t len = static_cast<std::uint16_t>(otMessageGetLength(msg) - off);
  if (len == 0 || len > sizeof buf) return;
  if (otMessageRead(msg, off, buf, len) != len) return;
  self->deliver(buf, len);
}

void ot_task(void* /*arg*/) {
  esp_openthread_launch_mainloop();  // runs until deinit; never returns in normal use
  esp_openthread_deinit();
  vTaskDelete(nullptr);
}

otLinkModeConfig mode_for(PowerProfile p) {
  otLinkModeConfig m{};
  m.mRxOnWhenIdle = p == PowerProfile::kAlwaysOn;
  m.mDeviceType = false;   // MTD: never a router
  m.mNetworkData = false;  // stable network data only
  return m;
}

}  // namespace

bool ThreadDatagram::start(const ThreadConfig& config, PowerProfile profile) {
  config_ = config;
  esp_vfs_eventfd_config_t eventfd = {};
  eventfd.max_fds = 3;
  if (esp_vfs_eventfd_register(&eventfd) != ESP_OK) return false;
  (void)esp_event_loop_create_default();  // ESP_ERR_INVALID_STATE if it exists: fine

  esp_openthread_platform_config_t platform = {};
  platform.radio_config.radio_mode = RADIO_MODE_NATIVE;
  platform.host_config.host_connection_mode = HOST_CONNECTION_MODE_NONE;
  platform.port_config.storage_partition_name = "nvs";
  platform.port_config.netif_queue_size = 10;
  platform.port_config.task_queue_size = 10;
  if (esp_openthread_init(&platform) != ESP_OK) {
    ESP_LOGE(kTag, "openthread init failed");
    return false;
  }

  esp_openthread_lock_acquire(portMAX_DELAY);
  otInstance* ot = esp_openthread_get_instance();
  otOperationalDatasetTlvs tlvs{};
  tlvs.mLength = config_.dataset_len;
  std::memcpy(tlvs.mTlvs, config_.dataset.data(), config_.dataset_len);
  bool ok = otDatasetSetActiveTlvs(ot, &tlvs) == OT_ERROR_NONE;
  ok = ok && otThreadSetLinkMode(ot, mode_for(profile)) == OT_ERROR_NONE;
  if (ok && profile != PowerProfile::kAlwaysOn) {
    ok = otLinkSetPollPeriod(ot, kSleepyPollMs) == OT_ERROR_NONE;
  }
  // The parent must keep the child through the longest deep sleep (the 1 h heartbeat
  // ceiling, NodeRuntime::kMaxHeartbeatIntervalMs), so a wake re-attaches with a
  // Child Update instead of a full attach.
  if (ok) otThreadSetChildTimeout(ot, kChildTimeoutS);
  ok = ok && otIp6SetEnabled(ot, true) == OT_ERROR_NONE;
  ok = ok && otThreadSetEnabled(ot, true) == OT_ERROR_NONE;
  ok = ok && otUdpOpen(ot, &g_socket, on_receive, this) == OT_ERROR_NONE;
  if (ok) {
    otSockAddr local{};
    local.mPort = kLocalPort;
    ok = otUdpBind(ot, &g_socket, &local, OT_NETIF_UNSPECIFIED) == OT_ERROR_NONE;
  }
  esp_openthread_lock_release();
  if (!ok) {
    ESP_LOGE(kTag, "thread configuration refused");
    return false;
  }
  socket_ = &g_socket;
  started_ = xTaskCreate(ot_task, "ot", 8192, nullptr, 5, nullptr) == pdPASS;
  ESP_LOGI(kTag, "talos thread started=%d hub=%s port=%u", started_ ? 1 : 0,
           config_.hub_address.data(), static_cast<unsigned>(config_.hub_port));
  return started_;
}

bool ThreadDatagram::attached() const {
  if (!started_) return false;
  esp_openthread_lock_acquire(portMAX_DELAY);
  const otDeviceRole role = otThreadGetDeviceRole(esp_openthread_get_instance());
  esp_openthread_lock_release();
  return role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER ||
         role == OT_DEVICE_ROLE_LEADER;
}

void ThreadDatagram::set_profile(PowerProfile profile) {
  if (!started_) return;
  esp_openthread_lock_acquire(portMAX_DELAY);
  otInstance* ot = esp_openthread_get_instance();
  (void)otThreadSetLinkMode(ot, mode_for(profile));
  if (profile != PowerProfile::kAlwaysOn) (void)otLinkSetPollPeriod(ot, kSleepyPollMs);
  esp_openthread_lock_release();
}

bool ThreadDatagram::send(const std::uint8_t* data, std::size_t n) {
  if (!started_ || n == 0 || n > kMaxDatagram) return false;
  esp_openthread_lock_acquire(portMAX_DELAY);
  otInstance* ot = esp_openthread_get_instance();
  bool ok = false;
  if (otMessage* msg = otUdpNewMessage(ot, nullptr)) {
    otMessageInfo info{};
    if (otIp6AddressFromString(config_.hub_address.data(), &info.mPeerAddr) == OT_ERROR_NONE &&
        otMessageAppend(msg, data, static_cast<std::uint16_t>(n)) == OT_ERROR_NONE) {
      info.mPeerPort = config_.hub_port;
      ok = otUdpSend(ot, &g_socket, msg, &info) == OT_ERROR_NONE;
      if (ok) msg = nullptr;  // owned by the stack now
    }
    if (msg) otMessageFree(msg);
  }
  esp_openthread_lock_release();
  return ok;
}

void ThreadDatagram::deliver(const std::uint8_t* data, std::size_t n) {
  const std::size_t head = head_.load(std::memory_order_relaxed);
  const std::size_t next = (head + 1) % kInboxSlots;
  if (next == tail_.load(std::memory_order_acquire)) return;  // full: the queue retries
  inbox_[head].len = static_cast<std::uint16_t>(n);
  std::memcpy(inbox_[head].data.data(), data, n);
  head_.store(next, std::memory_order_release);
}

std::size_t ThreadDatagram::receive(std::uint8_t* buf, std::size_t cap) {
  const std::size_t tail = tail_.load(std::memory_order_relaxed);
  if (tail == head_.load(std::memory_order_acquire)) return 0;
  const std::size_t n = inbox_[tail].len <= cap ? inbox_[tail].len : 0;  // too big: dropped
  if (n) std::memcpy(buf, inbox_[tail].data.data(), n);
  tail_.store((tail + 1) % kInboxSlots, std::memory_order_release);
  return n;
}

bool ThreadDatagram::set_tx_power_dbm(std::int8_t dbm) {
  if (!started_) return false;
  esp_openthread_lock_acquire(portMAX_DELAY);
  const bool ok = otPlatRadioSetTransmitPower(esp_openthread_get_instance(), dbm) == OT_ERROR_NONE;
  esp_openthread_lock_release();
  return ok;
}

}  // namespace talos::fw::esp
