// L2 twin backend: the real portable firmware (firmware/portable NodeRuntime) on the
// virtual board (firmware/sim), fed from the twin's world (bench-test-plan §3, ADR 0018).
//
// The world drives the board's physical inputs: reed contacts and the PIR's output line
// as GPIO levels, impacts as LIS2DW12 accelerations, zone temperature into the SHT40. The
// firmware decides what to report, debounces, masks warm-up, queues and retries; its
// DeviceEvents become envelopes through the same bridge the hub's CoAP ingress uses. The hub
// ACKs what it took, so an uplink lost in the twin's transport is retried by the firmware's
// own queue.
//
// The firmware has no actuators or camera yet, so devices with those stay on L1
// (unsupported_reason() says why).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "firmware/sim/virtual_board.h"
#include "simulation/base/rng.h"
#include "simulation/twin/backend.h"
#include "simulation/twin/devices.h"
#include "talos/sim/v1/site.pb.h"

namespace talos::sim {

class FirmwareDevice final : public IDeviceBackend {
 public:
  // Empty if the firmware can play this device; otherwise why not.
  static std::string unsupported_reason(const talos::sim::v1::SiteDevice& spec);

  FirmwareDevice(const talos::sim::v1::SiteDevice& spec, PropertyId property, std::uint64_t seed);

  const std::string& id() const override { return id_; }
  const char* level() const override { return "L2"; }
  void boot(std::int64_t t_ms, Timestamp now, const World& world,
            std::vector<Uplink>& out) override;
  void sample(std::int64_t t_ms, Timestamp now, World& world,
              const std::vector<std::pair<std::string, std::uint32_t>>& impacts,
              std::vector<Uplink>& out) override;
  void receive(std::int64_t t_ms, const Command& command, World& world) override;
  void delivered(const SourcePosition& position) override;
  void reboot(std::int64_t t_ms, Timestamp now, const World& world,
              std::vector<Uplink>& out) override;
  void set_clock_skew(std::int64_t skew_ms) override { skew_ms_ = skew_ms; }
  void set_fault(talos::sim::v1::ActuatorFault) override {}  // no actuators on L2

  fw::sim::Node& node() { return *node_; }

 private:
  struct Pir {
    std::uint8_t line;
    PirOptics optics;
  };
  struct Contact {
    std::uint8_t line;
    std::string portal;
  };
  struct Impact {
    std::uint8_t slot;
    talos::sim::v1::ImpactSpec spec;
  };
  struct Environment {
    std::uint8_t slot;
    talos::sim::v1::EnvironmentSpec spec;
  };

  void apply_inputs(const World& world);
  void power_on(Timestamp now, const World& world);
  void drain(std::vector<Uplink>& out);

  std::string id_;
  DeviceId device_;
  PropertyId property_;
  Vec2 position_;
  Rng rng_;
  std::unique_ptr<fw::sim::Node> node_;
  std::vector<Pir> pirs_;
  std::vector<Contact> contacts_;
  std::vector<Impact> impacts_;
  std::vector<Environment> environments_;
  fw::ConfigUpdate config_;
  Timestamp start_{0};  // hub time at node clock 0
  std::int64_t last_t_ms_ = 0;
  std::int64_t skew_ms_ = 0;
};

}  // namespace talos::sim
