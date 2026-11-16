// Device telemetry and health projection (spec §27, ADR 0014). This is the
// observability plane: bounded, in-memory, never the journal. It answers
// "is this device OK and what should the owner do", never alarm logic.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
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
  fw::PowerMode power_mode = fw::PowerMode::kNormal;
  std::map<std::uint8_t, SlotTelemetry> slots;
  std::optional<std::uint8_t> ota_state;  // fw::OtaState
  std::uint32_t ota_received = 0;
  std::uint32_t running_version = 0;
  std::uint32_t applied_config_revision = 0;
  std::deque<talos::v1::Diagnostic> recent;  // newest last, bounded
};

class DeviceRegistry {
 public:
  static constexpr std::size_t kRecentPerDevice = 64;

  void observe(const DeviceId& device, const fw::DeviceEvent& e, Timestamp received_at);
  const DeviceTelemetry* find(const DeviceId& device) const;
  std::vector<talos::v1::Diagnostic> diagnostics(const DeviceId& device) const;

 private:
  std::map<DeviceId, DeviceTelemetry> devices_;
};

// Pure projection. `telemetry` is null for devices that only use the HTTP
// device API; then freshness of their device.health capability decides.
talos::v1::Health project_health(const DeviceId& device, const DeviceTelemetry* telemetry,
                                 Freshness health_freshness,
                                 std::optional<Timestamp> health_last_seen, Timestamp now,
                                 Millis offline_after);

}  // namespace talos::server
