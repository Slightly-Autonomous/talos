// Portable firmware types (spec §35, ADR 0013). Embedded-friendly: no heap,
// no exceptions, no RTTI, fixed-size values only. Compiles for the host
// (Bazel tests, twin) and for the ESP32-H2 (ESP-IDF).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>

namespace talos::fw {

using TimeMs = std::uint64_t;

inline constexpr std::uint8_t kSlots = 4;
inline constexpr std::uint8_t kNoSlot = 0xFF;

// Delivery priority: P0 is never dropped while lower priorities exist.
enum class Priority : std::uint8_t { kP0Critical = 0, kP1State = 1, kP2Health = 2, kP3Debug = 3 };

enum class EventKind : std::uint8_t {
  kMotion,         // flag = detected
  kContact,        // flag = open; b = 1 for a state report (boot), 0 for a transition
  kImpact,         // a = peak mg, b = duration ms
  kEnvironment,    // a = temperature milli-C, b = humidity permille
  kHeartbeat,      // a = battery mV, b = power mode
  kModuleFault,    // a = ModuleFault code
  kModulePresent,  // a = module code (discovery report)
  kConfigApplied,  // a = applied config revision
  kOtaStatus,      // a = OtaState, b = next byte offset wanted
  kPowerMode,      // a = PowerMode
};

struct DeviceEvent {
  std::uint32_t boot_epoch = 0;  // persistent boot counter at emission time
  std::uint32_t seq = 0;         // per boot, starts at 1
  std::uint8_t slot = kNoSlot;
  EventKind kind = EventKind::kHeartbeat;
  Priority priority = Priority::kP2Health;
  TimeMs at_ms = 0;  // device monotonic time
  bool flag = false;
  std::int32_t a = 0;
  std::uint32_t b = 0;
  bool operator==(const DeviceEvent&) const = default;
};

enum class PowerMode : std::uint8_t { kNormal = 0, kLowPower = 1, kCritical = 2 };
enum class PowerSource : std::uint8_t { kBatteryOnly = 0, kUsb = 1, kSolarBattery = 2 };

enum class ModuleFault : std::int32_t {
  kNone = 0,
  kUnknownModule = 1,     // ID EEPROM read, code not in the registry
  kNoIdEeprom = 2,        // nothing answered on the slot's ID address
  kInitFailed = 3,        // driver probe failed (e.g. WHO_AM_I mismatch)
  kBusError = 4,          // I2C transaction failed at runtime
  kDisabledForPower = 5,  // present, but the power budget keeps it off
  kMissingResource = 6,   // slot lacks a wake line / bus the module needs
};

// Downlink messages from the hub.
struct Ack {
  std::uint32_t boot_epoch = 0;
  std::uint32_t seq = 0;
};
struct ConfigUpdate {
  std::uint32_t revision = 0;
  std::uint32_t heartbeat_interval_ms = 0;
  std::uint32_t environment_interval_ms = 0;
  std::uint32_t impact_threshold_mg = 0;
};
struct OtaOffer {
  std::uint32_t version = 0;
  std::uint32_t size = 0;
  std::array<std::uint8_t, 32> sha256{};
};
inline constexpr std::size_t kOtaChunkMax = 128;
struct OtaChunk {
  std::uint32_t offset = 0;
  std::uint16_t length = 0;
  std::array<std::uint8_t, kOtaChunkMax> data{};
};
struct RebootRequest {};

using Downlink = std::variant<Ack, ConfigUpdate, OtaOffer, OtaChunk, RebootRequest>;

}  // namespace talos::fw
