// Virtual devices: sensor behavior models and closed-loop actuators (spec §40).
//
// Phase 2 is the L1 behavioral twin: sensors turn world state into the same
// ObservationEnvelopes a real node would send. Electrical detail (GPIO edges,
// contact bounce, debounce, sleep) arrives with the portable firmware in
// Phase 5, which will replace the envelope-building here.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "core/events/observation.h"
#include "core/execution/command.h"
#include "simulation/base/geometry.h"
#include "simulation/base/rng.h"
#include "simulation/twin/world.h"
#include "talos/sim/v1/scenario.pb.h"
#include "talos/sim/v1/site.pb.h"

namespace talos::sim {

using UplinkBody = std::variant<ObservationEnvelope, CommandAck>;

// Device-to-hub messages produced during one tick.
struct Uplink {
  std::string device;
  UplinkBody body;
};

class VirtualDevice {
 public:
  VirtualDevice(const talos::sim::v1::SiteDevice& spec, PropertyId property, std::uint64_t seed);

  const std::string& id() const { return id_; }

  // Initial state reports at power-on.
  void boot(std::int64_t t_ms, Timestamp now, const World& world, std::vector<Uplink>& out);
  // Sensor sampling, heartbeats and actuator completions for this tick.
  void sample(std::int64_t t_ms, Timestamp now, World& world,
              const std::vector<std::pair<std::string, std::uint32_t>>& impacts,
              std::vector<Uplink>& out);
  // A command delivered by the transport.
  void receive(std::int64_t t_ms, const Command& command, World& world);

  void reboot(std::int64_t t_ms, Timestamp now, const World& world, std::vector<Uplink>& out);
  void set_clock_skew(std::int64_t skew_ms) { skew_ms_ = skew_ms; }
  void set_fault(talos::sim::v1::ActuatorFault fault) { fault_ = fault; }

 private:
  struct Pir {
    std::uint8_t slot;
    talos::sim::v1::PirSpec spec;
    Wedge wedge;
    bool active = false;
    bool suppressed = false;  // current episode missed
    std::int64_t hold_until = 0;
    std::map<std::string, bool> pet_visible;
  };
  struct Contact {
    std::uint8_t slot;
    std::string portal;
    std::optional<bool> last;
  };
  struct Impact {
    std::uint8_t slot;
    talos::sim::v1::ImpactSpec spec;
  };
  struct Environment {
    std::uint8_t slot;
    talos::sim::v1::EnvironmentSpec spec;
    std::int64_t next_at = 0;
  };
  struct Camera {
    std::uint8_t slot;
    talos::sim::v1::CameraSpec spec;
    Wedge wedge;
    std::int64_t last_emit = -1'000'000'000;
  };
  struct PendingAction {
    std::int64_t due_ms;
    Command command;
  };

  ObservationEnvelope envelope(Timestamp now, std::optional<std::uint8_t> slot,
                               const std::string& capability, std::string event_type,
                               ObservationPayload payload);
  void emit(std::vector<Uplink>& out, ObservationEnvelope env) {
    out.push_back({id_, std::move(env)});
  }
  void report_initial_state(Timestamp now, const World& world, std::vector<Uplink>& out);
  void complete(std::int64_t t_ms, Timestamp now, const PendingAction& action, World& world,
                std::vector<Uplink>& out);

  std::string id_;
  PropertyId property_;
  Vec2 position_;
  Rng rng_;
  std::uint32_t boot_epoch_ = 1;
  std::uint64_t sequence_ = 0;
  std::int64_t skew_ms_ = 0;
  std::int64_t boot_ms_ = 0;
  std::uint32_t heartbeat_interval_ms_ = 0;
  std::uint32_t battery_mv_ = 0;
  std::int64_t next_heartbeat_ = 0;
  std::vector<Pir> pirs_;
  std::vector<Contact> contacts_;
  std::vector<Impact> impacts_;
  std::vector<Environment> environments_;
  std::vector<Camera> cameras_;
  std::optional<talos::sim::v1::SpeakerSpec> speaker_;
  std::optional<talos::sim::v1::LockSpec> lock_;
  std::optional<talos::sim::v1::NotifierSpec> notifier_;
  talos::sim::v1::ActuatorFault fault_ = talos::sim::v1::ACTUATOR_FAULT_NONE;
  std::vector<PendingAction> pending_;
  std::set<std::string> executed_;  // device-side idempotency by command id
};

}  // namespace talos::sim
