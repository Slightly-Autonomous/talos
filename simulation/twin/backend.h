// The seam between the twin's world and whatever plays a device (bench-test-plan §3):
//   L1 behavioural  VirtualDevice (devices.h)
//   L2 firmware     FirmwareDevice (firmware_device.h): the real portable firmware on the
//                   virtual board, fed from the same world
//   L4 hardware     a bench node; not runnable in the twin yet, so the runner skips
// The runner talks to every device through this interface only, so one scenario runs
// unchanged on any backend and the parity test can hold L1 to L2 (ADR 0018).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "core/events/observation.h"
#include "core/execution/command.h"
#include "simulation/twin/world.h"
#include "talos/sim/v1/scenario.pb.h"

namespace talos::sim {

using UplinkBody = std::variant<ObservationEnvelope, CommandAck>;

// Device-to-hub messages produced during one tick.
struct Uplink {
  std::string device;
  UplinkBody body;
};

class IDeviceBackend {
 public:
  virtual ~IDeviceBackend() = default;
  virtual const std::string& id() const = 0;
  // "L1" or "L2", for reports.
  virtual const char* level() const = 0;

  // Scenario start. The device is already installed and running, past any sensor warm-up,
  // and reports its state as it would on joining the hub.
  virtual void boot(std::int64_t t_ms, Timestamp now, const World& world,
                    std::vector<Uplink>& out) = 0;
  // Advance one twin step: read the world, run the device, collect its uplinks.
  virtual void sample(std::int64_t t_ms, Timestamp now, World& world,
                      const std::vector<std::pair<std::string, std::uint32_t>>& impacts,
                      std::vector<Uplink>& out) = 0;
  // A command delivered by the transport.
  virtual void receive(std::int64_t t_ms, const Command& command, World& world) = 0;
  // The hub took this uplink (accepted or duplicate). L2 turns it into the firmware's ACK, so
  // lost uplinks are retried by the firmware's own queue rather than by the twin.
  virtual void delivered(const SourcePosition& position) { (void)position; }

  // A real power-on now: warm-up applies from here (L1: PirSpec.warmup_ms; L2: the
  // firmware's own mask).
  virtual void reboot(std::int64_t t_ms, Timestamp now, const World& world,
                      std::vector<Uplink>& out) = 0;
  virtual void set_clock_skew(std::int64_t skew_ms) = 0;
  virtual void set_fault(talos::sim::v1::ActuatorFault fault) = 0;
};

}  // namespace talos::sim
