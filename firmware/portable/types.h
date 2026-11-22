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
  // Tamper (HD-032, HD-049), slot = kNoSlot. flag = open / active; a =
  // TamperCause (0 = the enclosure loop, as before 2026-11-22); b = 1 for a
  // state report (cold boot), 0 for a transition. Opening is P0, a restore P1.
  // Appended: never renumber the kinds above.
  kTamper,
  // USER button (TL-SN-002, ADR 0019), slot = kNoSlot, P1. a = UserButtonAction.
  // A short press also sends a heartbeat (the test report) and runs the lamp
  // test; a long press asks the hub to commission or pair the node. Appended.
  kUserButton,
  // USB console refusals (TL-SN-002, BoardSupport::console_lock), slot = kNoSlot,
  // P1, a diagnostic and never an alarm. a = commands refused since the last
  // report (>= 1), b = refused since boot. At most one per kConsoleReportMs; the
  // first refusal is reported at once. Appended.
  kConsoleLocked,
  // Why this boot happened (TL-SN-002, owner 2026-11-22: a reset through the
  // outside USB-C port must be visible). slot = kNoSlot, P1 diagnostic, sent
  // once per boot that is not a deep-sleep wake. a = ResetCause, b = 1 if the
  // enclosure was closed at boot. Appended.
  kReset,
};

// EventKind::kTamper `a`: which tamper sensor. Each maps to its own hub
// capability (firmware/bridge/hub_bridge.cc).
enum class TamperCause : std::int32_t {
  kEnclosure = 0,     // lid switch or the loop (cut, or J9's external switches)
  kBatteryDoor = 1,   // the tool-free battery door (SW5, TAMPER_DOOR)
  kMountMoved = 2,    // the accelerometer's gravity vector left the installed pose
  kMagnetDefeat = 3,  // a foreign magnet: field far above or away from the closed baseline
};

// How the node sleeps between reports (owner 2026-11-22: default deep sleep
// with <= 2 s event latency; some installs need instant). ConfigUpdate and
// BoardSupport::power_profile. USB power always runs kAlwaysOn.
enum class PowerProfile : std::uint8_t {
  kDeepSleep = 0,    // deep sleep, rejoin on wake: lowest current, event-to-hub <= ~2 s
  kSleepyChild = 1,  // light sleep, stays attached (Thread SED): ~100 ms, ~3-4x the current
  kAlwaysOn = 2,     // radio always on (MED/FTD): instant, USB or mains only
};

// EventKind::kUserButton `a`.
enum class UserButtonAction : std::int32_t {
  kTestReport = 1,         // short press: "is this node alive and heard?"
  kCommissionRequest = 2,  // long press (>= UserButton::kLongPressMs): commission / pair
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

// Board-level debug lamps besides the per-slot (per-sensor) ones (TL-SN-002,
// ADR 0019). Each is an expander pin, active low (BoardSupport::lamp_n).
enum class Lamp : std::uint8_t {
  kLink = 0,     // Thread/hub state: solid = joined, slow blink = joining or failing
  kTamper = 1,   // enclosure loop open
  kBattery = 2,  // cells low or missing
  kFault = 3,    // any fault (sensor, board, epoch) in the last kFaultLampMs
};
inline constexpr std::size_t kLampCount = 4;

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
  // Appended 2026-11-22 (wire: optional trailing byte). kKeepProfile = unchanged.
  static constexpr std::uint8_t kKeepProfile = 0xFF;
  std::uint8_t power_profile = kKeepProfile;
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
