#include "firmware/bridge/hub_bridge.h"

#include <string>

namespace talos::fw {

const char* tamper_capability(TamperCause c) {
  switch (c) {
    case TamperCause::kBatteryDoor:
      return "tamper.battery_door";
    case TamperCause::kMountMoved:
      return "tamper.mount";
    case TamperCause::kMagnetDefeat:
      return "tamper.magnet";
    case TamperCause::kEnclosure:
      break;
  }
  return "tamper.enclosure";  // the loop, and any cause this hub does not know yet
}

std::optional<ObservationEnvelope> to_envelope(const DeviceEvent& e, const DeviceId& device,
                                               const PropertyId& property) {
  ObservationEnvelope env;
  env.property = property;
  env.source.device = device;
  if (e.slot != kNoSlot) env.source.slot = e.slot;
  env.position = {e.boot_epoch, e.seq};
  env.event_id =
      EventId(device.str() + ":" + std::to_string(e.boot_epoch) + ":" + std::to_string(e.seq));
  switch (e.kind) {
    case EventKind::kMotion:
      env.source.capability = CapabilityId("motion.detect");
      env.event_type = e.flag ? "motion.detected" : "motion.cleared";
      env.payload = MotionState{e.flag};
      break;
    case EventKind::kContact:
      env.source.capability = CapabilityId("contact.open_state");
      env.event_type = e.b == 1 ? "contact.state" : (e.flag ? "contact.opened" : "contact.closed");
      env.payload = ContactState{e.flag};
      break;
    case EventKind::kImpact:
      env.source.capability = CapabilityId("impact.detect");
      env.event_type = "impact.detected";
      env.payload = ImpactDetected{e.a, e.b};
      break;
    case EventKind::kEnvironment:
      env.source.capability = CapabilityId("environment.temperature_humidity");
      env.event_type = "environment.reading";
      env.payload = TemperatureHumidity{e.a, e.b};
      break;
    case EventKind::kHeartbeat:
      env.source.slot.reset();
      env.source.capability = CapabilityId("device.health");
      env.event_type = "device.heartbeat";
      env.payload = Heartbeat{static_cast<std::uint32_t>(e.a)};
      break;
    case EventKind::kTamper:
      // Device-level, no slot: zones come from the device's placement (ADR 0011).
      env.source.slot.reset();
      env.source.capability = CapabilityId(tamper_capability(static_cast<TamperCause>(e.a)));
      env.event_type = e.b == 1 ? "tamper.state" : (e.flag ? "tamper.opened" : "tamper.closed");
      env.payload = TamperState{e.flag};
      break;
    default:
      return std::nullopt;
  }
  return env;
}

}  // namespace talos::fw
