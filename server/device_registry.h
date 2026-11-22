// Device telemetry and health projection (spec §27, ADR 0014). This is the
// observability plane: bounded, in-memory, never the journal. It answers
// "is this device OK and what should the owner do", never alarm logic.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/domain/ids.h"
#include "core/state/state.h"
#include "core/time/time.h"
#include "firmware/portable/types.h"
#include "talos/v1/health.pb.h"

namespace talos::server {

struct SlotTelemetry {
  std::uint16_t module_code = 0;
  fw::ModuleFault fault = fw::ModuleFault::kNone;
};

struct DeviceTelemetry {
  std::optional<Timestamp> last_seen;
  std::uint32_t battery_mv = 0;
  bool heartbeat_seen = false;  // battery_mv is a report, not the default
  fw::PowerMode power_mode = fw::PowerMode::kNormal;
  std::map<std::uint8_t, SlotTelemetry> slots;
  std::optional<std::uint8_t> ota_state;  // fw::OtaState
  std::uint32_t ota_received = 0;
  std::uint32_t running_version = 0;
  std::uint32_t applied_config_revision = 0;
  // Enclosure tamper loop (fw::EventKind::kTamper): true = lid open or loop
  // cut, nullopt = never reported (nodes without a tamper input).
  std::optional<bool> tamper_open;
  std::optional<std::pair<std::uint32_t, std::uint32_t>> tamper_pos;  // (boot_epoch, seq) of it
  // USER button (TL-SN-002, fw::EventKind::kUserButton): when the owner last
  // asked for a test report / for commissioning. Diagnostics only: the hub
  // never pairs or changes anything on its own because of it.
  std::optional<Timestamp> last_test_report;
  std::optional<Timestamp> commission_requested;
  std::deque<talos::v1::Diagnostic> recent;  // newest last, bounded
  // Recent (boot_epoch, seq) of applied non-journaled events, bounded; the
  // journal's dedup covers the other kinds.
  std::set<std::pair<std::uint32_t, std::uint32_t>> unjournaled_seen;
};

// Reports the hub had to ignore because they sit behind (or jump too far past)
// what it holds for the device: the signature of a poisoned boot epoch or a
// reflashed device. Health shows this instead of "healthy".
struct SourceAnomaly {
  std::uint32_t behind = 0;        // accepted, but older boot than the held position
  std::uint32_t out_of_range = 0;  // refused: epoch or sequence jumped too far
  std::optional<Timestamp> last;
};

class DeviceRegistry {
 public:
  static constexpr std::size_t kRecentPerDevice = 64;
  static constexpr std::size_t kSeenPerDevice = 256;

  // Applies an event the caller has already established as fresh (for
  // journaled kinds: ingest accepted it). Never call it for duplicates.
  void observe(const DeviceId& device, const fw::DeviceEvent& e, Timestamp received_at);
  // For kinds the journal never sees (OTA, config, faults, power): applies
  // the event only if its (boot_epoch, seq) was not applied before and is not
  // older than the retained window, so a replayed frame cannot refresh
  // last_seen, battery or OTA state. Reordering inside the window is fine.
  // Returns whether it was applied.
  bool observe_unjournaled(const DeviceId& device, const fw::DeviceEvent& e, Timestamp received_at);
  // Whether observe_unjournaled would apply this event (read-only).
  bool unjournaled_is_new(const DeviceId& device, const fw::DeviceEvent& e) const;
  void note_behind(const DeviceId& device, Timestamp at);
  void note_out_of_range(const DeviceId& device, Timestamp at);
  void clear_anomaly(const DeviceId& device) { anomalies_.erase(device); }
  const SourceAnomaly* anomaly(const DeviceId& device) const;
  const DeviceTelemetry* find(const DeviceId& device) const;
  std::vector<talos::v1::Diagnostic> diagnostics(const DeviceId& device) const;

 private:
  std::map<DeviceId, DeviceTelemetry> devices_;
  std::map<DeviceId, SourceAnomaly> anomalies_;
};

// Pure projection. `telemetry` is null for devices that only use the HTTP
// device API; then freshness of their device.health capability decides.
// A recent SourceAnomaly (within offline_after) reports the device FAULTED:
// its real reports are being ignored.
talos::v1::Health project_health(const DeviceId& device, const DeviceTelemetry* telemetry,
                                 Freshness health_freshness,
                                 std::optional<Timestamp> health_last_seen, Timestamp now,
                                 Millis offline_after, const SourceAnomaly* anomaly = nullptr,
                                 bool tamper_open = false,
                                 std::optional<Timestamp> maintenance_until = std::nullopt,
                                 bool primary_cells = false, bool built_in_sensors = false);

// Boards whose battery is primary cells (never charged): low battery means
// "replace the cells", not "charge". TL-SN-002 (ADR 0019): 2 x AA Li-FeS2.
bool board_has_primary_cells(const std::optional<BoardId>& board);
// Boards whose sensors are soldered on (ADR 0019): a sensor fault can't be fixed
// by reseating a card, and a slot index names a built-in sensor.
bool board_has_built_in_sensors(const std::optional<BoardId>& board);

}  // namespace talos::server
