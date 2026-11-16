// Portable firmware + virtual board + the real HubEngine: the L2 twin.
#include <gtest/gtest.h>

#include "core/engine/engine.h"
#include "core/testing/fixtures.h"
#include "firmware/sim/hub_bridge.h"
#include "firmware/sim/virtual_board.h"

namespace talos::fw::sim {
namespace {

class NodeAndHub : public ::testing::Test {
 protected:
  NodeAndHub() : node(default_board()) {
    EngineConfig cfg;
    cfg.property = talos::testing::sample_property();
    cfg.freshness_max_age = Millis{120'000};
    engine = std::make_unique<HubEngine>(cfg, journal, hub_clock);
    node.insert(0, ModuleCode::kReedContact);  // node-12 slot 0: front door
  }

  // Delivers the node's uplink to the hub, ACKing what the hub accepted or
  // already had (duplicates) - exactly what the Phase 6 adapter will do.
  void pump(TimeMs ms) {
    for (TimeMs t = 0; t < ms; t += 10) {
      node.run_for(10);
      hub_clock.advance(Millis{10});
      while (!node.link.uplink.empty()) {
        const DeviceEvent e = node.link.uplink.front();
        node.link.uplink.pop_front();
        if (auto env =
                to_envelope(e, node.runtime(), DeviceId("node-12"), PropertyId("house-01"))) {
          auto r = engine->on_observation(*env);
          ASSERT_NE(r.status, IngestStatus::kRejected) << r.error->message;
          if (r.status == IngestStatus::kDuplicate) ++duplicates;
        }
        node.link.downlink.push_back(Ack{e.boot_epoch, e.seq});
      }
    }
  }

  std::size_t journaled(const std::string& event_type) {
    std::size_t n = 0;
    for (const auto& entry : read_all(journal)) {
      if (const auto* o = std::get_if<ObservationRecorded>(&entry.event)) {
        n += o->envelope.event_type == event_type ? 1 : 0;
      }
    }
    return n;
  }

  Node node;
  InMemoryJournal journal;
  VirtualClock hub_clock{Timestamp{1'800'000'000'000}};
  std::unique_ptr<HubEngine> engine;
  int duplicates = 0;
};

const CapabilityRef kDoor{DeviceId("node-12"), SlotIndex{0}, CapabilityId("contact.open_state")};

TEST_F(NodeAndHub, BouncyDoorBecomesOneJournaledOpening) {
  node.boot();
  pump(50);
  for (int i = 0; i < 8; ++i) {  // chatter, then settle open
    node.gpio.set(0, i % 2 == 0);
    pump(10);
  }
  node.gpio.set(0, true);
  pump(200);
  EXPECT_EQ(journaled("contact.opened"), 1u);
  EXPECT_EQ(std::get<ContactState>(engine->state().capabilities.at(kDoor).value).open, true);
  EXPECT_EQ(
      engine->freshness_of({DeviceId("node-12"), std::nullopt, CapabilityId("device.health")}),
      Freshness::kFresh);
}

TEST_F(NodeAndHub, LinkOutageDelaysButNeverDuplicatesTheEvent) {
  node.boot();
  pump(50);
  node.link.connected = false;
  node.gpio.set(0, true);
  pump(5'000);  // retries back off while the radio is down
  EXPECT_EQ(journaled("contact.opened"), 0u);
  node.link.connected = true;
  pump(10'000);
  EXPECT_EQ(journaled("contact.opened"), 1u);
}

TEST_F(NodeAndHub, CriticalEventSurvivesNodeRebootAndHubDedupes) {
  node.boot();
  pump(50);
  node.link.drop_next = 1000;  // uplink lost, but the hub's ACKs are not the issue
  node.gpio.set(0, true);
  pump(100);
  node.boot();  // power loss before delivery
  node.link.drop_next = 0;
  pump(10'000);
  EXPECT_EQ(journaled("contact.opened"), 1u);
  // The restored copy kept its original (boot_epoch, seq); a second delivery
  // of it would be a hub duplicate, never a second opening.
  EXPECT_EQ(std::get<ContactState>(engine->state().capabilities.at(kDoor).value).open, true);
}

}  // namespace
}  // namespace talos::fw::sim
