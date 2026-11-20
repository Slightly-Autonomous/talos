#include "core/testing/fixtures.h"

namespace talos::testing {

Property sample_property() {
  Property p;
  p.id = PropertyId("house-01");
  p.timezone = "America/Los_Angeles";
  p.zones = {{ZoneId("backyard"), "Backyard", ZoneKind::kOutdoor},
             {ZoneId("patio"), "Patio", ZoneKind::kOutdoor},
             {ZoneId("hallway"), "Hallway", ZoneKind::kIndoor}};
  // Capabilities are declared explicitly: an empty list declares none.
  auto add = [&](std::string id, std::string zone, std::vector<std::string> caps) {
    Device d;
    d.id = DeviceId(std::move(id));
    d.property = p.id;
    d.zone = ZoneId(std::move(zone));
    for (auto& c : caps) d.capabilities.emplace_back(std::move(c));
    p.devices.push_back(d);
  };
  add("node-07", "backyard",
      {"motion.detect", "impact.detect", "camera.detection", "environment.temperature_humidity"});
  add("node-12", "hallway",
      {"contact.open_state", "motion.detect", "impact.detect", "environment.temperature_humidity"});
  add("patio_speaker", "patio", {"audio.speak"});
  add("lock-3", "hallway", {"access.lock"});
  Asset door;
  door.id = AssetId("front_door");
  door.type = "portal.door";
  door.zone = ZoneId("hallway");
  door.attached = {{DeviceId("node-12"), SlotIndex{0}, CapabilityId("contact.open_state")},
                   {DeviceId("lock-3"), std::nullopt, CapabilityId("access.lock")}};
  p.assets.push_back(door);
  return p;
}

namespace {
ObservationEnvelope base(const std::string& device, std::uint32_t boot, std::uint64_t seq) {
  ObservationEnvelope e;
  e.property = PropertyId("house-01");
  e.event_id = EventId(device + ":" + std::to_string(boot) + ":" + std::to_string(seq));
  e.position = {boot, seq};
  e.source.device = DeviceId(device);
  return e;
}
}  // namespace

ObservationEnvelope motion(const std::string& device, std::uint32_t boot, std::uint64_t seq,
                           bool detected) {
  auto e = base(device, boot, seq);
  e.source.slot = SlotIndex{0};
  e.source.capability = CapabilityId("motion.detect");
  e.event_type = detected ? "motion.detected" : "motion.cleared";
  e.payload = MotionState{detected};
  return e;
}

ObservationEnvelope contact(const std::string& device, std::uint32_t boot, std::uint64_t seq,
                            bool open) {
  auto e = base(device, boot, seq);
  e.source.slot = SlotIndex{0};
  e.source.capability = CapabilityId("contact.open_state");
  e.event_type = open ? "contact.opened" : "contact.closed";
  e.payload = ContactState{open};
  return e;
}

}  // namespace talos::testing
