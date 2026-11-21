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
  kOtaStatus,      // a = OtaState, b = next byte wanted (downloading), else running version
  kPowerMode,      // a = PowerMode
  // Enclosure tamper loop (HD-032), slot = kNoSlot. flag = open (lid open or
  // loop cut); b = 1 for a state report (cold boot), 0 for a transition.
  // Opening is P0, a restore P1. Appended: never renumber the kinds above.
  kTamper,
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
// kUnknown: the source could not be read (expander or charger not answering).
// Treated like battery-only for budgets, and reported as unknown, never as USB.
enum class PowerSource : std::uint8_t {
  kBatteryOnly = 0,
  kUsb = 1,
  kSolarBattery = 2,
  kUnknown = 3
};

enum class ModuleFault : std::int32_t {
  kNone = 0,
  kUnknownModule = 1,      // ID EEPROM read, code not in the registry
  kNoIdEeprom = 2,         // nothing answered on the slot's ID address
  kInitFailed = 3,         // driver probe failed (e.g. WHO_AM_I mismatch)
  kBusError = 4,           // I2C transaction failed at runtime
  kDisabledForPower = 5,   // present, but the power budget keeps it off
  kMissingResource = 6,    // slot lacks a wake line / bus the module needs
  kMuxFault = 7,           // the I2C mux did not accept the branch select
  kIdBlank = 8,            // ID EEPROM answered but was never programmed (all 0xFF)
  kIdCorrupt = 9,          // ID EEPROM content invalid (bad CRC, format or code 0)
  kStuckInterrupt = 10,    // wake line held high with no event behind it
  kWarmupTimeout = 11,     // sensor never settled after power-up (PIR: > 210 s)
  kRailFault = 12,         // the slot's switched rail hit its current limit and was shut off
  kPowerQuarantined = 13,  // the node reset while powering this slot; held off until cleared
  kCardRemoved = 14,    // a card that was running is gone (PRESENT_N or its ID stopped answering)
  kWakeLineFault = 15,  // the sensor latched an event but its WAKE line never rose (stuck low/open)
  // Node-level tamper is not a module fault: see EventKind::kTamper.
};
// kBusError with b = kFaultDetailUnarmed: a sensor that could not be re-armed
// after bounded retries (parked in sleep). A detail, not a new ModuleFault, so
// hub code that switches over ModuleFault needs no change.
inline constexpr std::uint32_t kFaultDetailUnarmed = 0x10;
// Supervision faults on a critical slot (stuck WAKE, removal, quarantine, a
// sensor that can't be re-armed) mean intrusion sensing is lost: the runtime
// sends them as P0 kModuleFault events. Same wire kind and 28-byte layout as
// before, so an older hub still decodes them; it only has to treat a P0
// module fault as a trouble condition (docs/decisions/0013 amendment).

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
