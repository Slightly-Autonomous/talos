// Thread network credentials and the hub's address (provisioned with diag
// `thread <dataset-tlvs-hex> <hub-ipv6> [port]`, lid open). The ESP32-H2 build
// starts OpenThread with them and runs the CoAP link over Thread UDP; without
// them the node talks over the USB console (bench). PROVISIONAL until the
// commissioning flow (spec §37) replaces manual provisioning.
//
// This is a network credential: never commit a real dataset (CLAUDE.md).
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

#include "firmware/portable/hal.h"

namespace talos::fw {

struct ThreadConfig {
  static constexpr std::size_t kMaxDataset = 254;  // OT_OPERATIONAL_DATASET_MAX_LENGTH
  static constexpr std::size_t kMaxAddress = 45;   // textual IPv6
  static constexpr std::uint16_t kDefaultPort = 5683;
  std::uint8_t version = 1;
  std::uint8_t dataset_len = 0;
  std::array<std::uint8_t, kMaxDataset> dataset{};
  std::array<char, kMaxAddress + 1> hub_address{};
  std::uint16_t hub_port = kDefaultPort;
  bool valid() const { return version == 1 && dataset_len > 0 && hub_address[0] != 0; }
};

inline std::optional<ThreadConfig> load_thread_config(IStore& store) {
  ThreadConfig c;
  const auto n = store.get(StoreKey::kThreadConfig, reinterpret_cast<std::uint8_t*>(&c), sizeof c);
  if (!n || *n != sizeof c || !c.valid()) return std::nullopt;
  c.hub_address.back() = 0;
  return c;
}

inline bool save_thread_config(IStore& store, const ThreadConfig& c) {
  return store.put(StoreKey::kThreadConfig, reinterpret_cast<const std::uint8_t*>(&c), sizeof c);
}

}  // namespace talos::fw
