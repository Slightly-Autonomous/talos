// Observation envelope: the normalized form of every piece of sensor evidence
// entering the hub (spec §22). Units are explicit in field names.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "core/base/result.h"
#include "core/domain/capability.h"
#include "core/domain/ids.h"
#include "core/time/time.h"

namespace talos {

struct ContactState {
  bool open = false;
  auto operator<=>(const ContactState&) const = default;
};
struct MotionState {
  bool detected = false;
  auto operator<=>(const MotionState&) const = default;
};
struct ImpactDetected {
  std::int32_t peak_mg = 0;
  std::uint32_t duration_ms = 0;
  auto operator<=>(const ImpactDetected&) const = default;
};
struct TemperatureHumidity {
  std::int32_t temperature_milli_c = 0;
  std::uint32_t humidity_permille = 0;
  auto operator<=>(const TemperatureHumidity&) const = default;
};
struct LockState {
  bool locked = false;
  auto operator<=>(const LockState&) const = default;
};
struct AudioPlayback {
  bool playing = false;
  auto operator<=>(const AudioPlayback&) const = default;
};

// Camera/vision detection metadata (never pixels).
struct ObjectDetected {
  std::string label;  // e.g. "person"
  std::uint16_t confidence_permille = 0;
  auto operator<=>(const ObjectDetected&) const = default;
};
// Enclosure tamper loop (capability "tamper.enclosure"): open = lid open or
// loop cut. A security event in every arm mode (ADR 0007 amendment).
struct TamperState {
  bool open = false;
  auto operator<=>(const TamperState&) const = default;
};
// Periodic device supervision report.
struct Heartbeat {
  std::uint32_t battery_mv = 0;
  auto operator<=>(const Heartbeat&) const = default;
};

using ObservationPayload =
    std::variant<std::monostate, ContactState, MotionState, ImpactDetected, TemperatureHumidity,
                 LockState, AudioPlayback, ObjectDetected, Heartbeat, TamperState>;

// Per-device source position. `boot_epoch` is a persistent counter the device
// increments on every boot; `sequence` restarts at each boot. Together they
// identify an event at its source. They do NOT order events across devices;
// the hub's journal_seq does that (ADR 0008).
struct SourcePosition {
  std::uint32_t boot_epoch = 0;
  std::uint64_t sequence = 0;
  auto operator<=>(const SourcePosition&) const = default;
};

struct ObservationEnvelope {
  std::uint32_t schema_version = 1;
  PropertyId property;
  CapabilityRef source;
  std::optional<ModuleId> module;
  std::string event_type;  // e.g. "motion.detected"
  EventId event_id;        // stable across transport retries
  SourcePosition position;
  // Device clock; may be wrong or absent. Never used for ordering.
  std::optional<Timestamp> observed_at;
  ObservationPayload payload;
  std::optional<EventId> causation;
  bool operator==(const ObservationEnvelope&) const = default;
};

inline constexpr std::size_t kMaxEventIdLen = 128;
inline constexpr std::size_t kMaxEventTypeLen = 64;
inline constexpr std::size_t kMaxLabelLen = 64;

// Structural validation of an envelope arriving from any adapter: required
// fields, and bounded lengths and character sets for device-controlled text.
Status validate_envelope(const ObservationEnvelope& env);

}  // namespace talos
