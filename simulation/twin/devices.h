// Virtual devices: sensor behavior models and closed-loop actuators (spec §40).
//
// The L1 behavioural twin: sensors turn world state into the same
// ObservationEnvelopes a real node would send. Electrical detail (GPIO edges,
// contact bounce, debounce, sleep) lives in the L2 backend, which runs the real
// portable firmware instead (firmware_device.h); the parity test holds this
// model to it.
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
#include "simulation/twin/backend.h"
#include "simulation/twin/world.h"
#include "talos/sim/v1/scenario.pb.h"
#include "talos/sim/v1/site.pb.h"

namespace talos::sim {

// A PIR's optics and output stage: which moving actors it sees through walls and its wedge,
// pets it may ignore, misses, false triggers and the output hold. L1 turns its edges into
// envelopes; L2 drives the EKMB output line with it and lets the firmware do the rest.
class PirOptics {
 public:
  PirOptics(Vec2 position, const talos::sim::v1::PirSpec& spec);
  // The sensor output after this tick (true while detecting or holding).
  bool step(std::int64_t t_ms, const World& world, Rng& rng);
  void reset() {
    active_ = false;
    suppressed_ = false;
  }
  bool active() const { return active_; }
  const talos::sim::v1::PirSpec& spec() const { return spec_; }

 private:
  Vec2 position_;
  talos::sim::v1::PirSpec spec_;
  Wedge wedge_;
  bool active_ = false;
  bool suppressed_ = false;  // current episode missed
  std::int64_t hold_until_ = 0;
  std::map<std::string, bool> pet_visible_;
};

class VirtualDevice final : public IDeviceBackend {
 public:
  VirtualDevice(const talos::sim::v1::SiteDevice& spec, PropertyId property, std::uint64_t seed);

  const std::string& id() const override { return id_; }
  const char* level() const override { return "L1"; }

  // Initial state reports at power-on.
  void boot(std::int64_t t_ms, Timestamp now, const World& world,
            std::vector<Uplink>& out) override;
  // Sensor sampling, heartbeats and actuator completions for this tick.
  void sample(std::int64_t t_ms, Timestamp now, World& world,
              const std::vector<std::pair<std::string, std::uint32_t>>& impacts,
              std::vector<Uplink>& out) override;
  // A command delivered by the transport.
  void receive(std::int64_t t_ms, const Command& command, World& world) override;

  void reboot(std::int64_t t_ms, Timestamp now, const World& world,
              std::vector<Uplink>& out) override;
  void set_clock_skew(std::int64_t skew_ms) override { skew_ms_ = skew_ms; }
  void set_fault(talos::sim::v1::ActuatorFault fault) override { fault_ = fault; }

 private:
  struct Pir {
    std::uint8_t slot;
    PirOptics optics;
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
  void start(std::int64_t t_ms, Timestamp now, const World& world, std::vector<Uplink>& out);
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
