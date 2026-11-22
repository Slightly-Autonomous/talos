// Thread transport for the CoAP link (ADR 0014, ADR 0019 amendment 2026-11-22):
// IDatagram over OpenThread UDP on the ESP32-H2's native 802.15.4 radio.
//
// The node is a Minimal Thread Device. Its link mode follows the power profile:
//   kDeepSleep / kSleepyChild: sleepy end device (rx off when idle), polling its
//     parent every kSleepyPollMs while awake so piggybacked replies arrive;
//   kAlwaysOn: rx-on-when-idle end device (MED): instant.
// The operational dataset and the hub's address come from ThreadConfig
// (diag `thread`). OpenThread keeps its own settings (child info) in the "nvs"
// partition, so a deep-sleep wake re-attaches with a Child Update instead of a
// full attach.
//
// PROVISIONAL: compile-checked in CI; Thread attach, polling and latency are
// unverified until the bench (OTNS harness for the logic: tools/otns).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "firmware/portable/hal.h"
#include "firmware/portable/thread_config.h"

namespace talos::fw::esp {

class ThreadDatagram final : public IDatagram {
 public:
  static constexpr std::uint32_t kSleepyPollMs = 250;
  static constexpr std::uint32_t kChildTimeoutS = 4'000;
  static constexpr std::size_t kInboxSlots = 8;
  static constexpr std::size_t kMaxDatagram = 1280;

  // Starts OpenThread (its own task) with the stored dataset. Returns false if
  // the stack could not start; the caller then falls back to the console link.
  bool start(const ThreadConfig& config, PowerProfile profile);
  bool started() const { return started_; }
  // Attached to a parent (child role or better) and able to reach the hub.
  bool attached() const;
  void set_profile(PowerProfile profile);

  bool send(const std::uint8_t* data, std::size_t n) override;
  std::size_t receive(std::uint8_t* buf, std::size_t cap) override;
  bool set_tx_power_dbm(std::int8_t dbm) override;

  // Called from the OpenThread task with a received datagram.
  void deliver(const std::uint8_t* data, std::size_t n);

 private:
  struct Slot {
    std::uint16_t len = 0;
    std::array<std::uint8_t, kMaxDatagram> data{};
  };
  bool started_ = false;
  ThreadConfig config_{};
  // Single-producer (OT task) / single-consumer (app_main) ring.
  std::array<Slot, kInboxSlots> inbox_{};
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
  void* socket_ = nullptr;  // otUdpSocket*, opaque here to keep OT headers out
};

}  // namespace talos::fw::esp
