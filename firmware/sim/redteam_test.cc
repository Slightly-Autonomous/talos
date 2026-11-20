// Regression tests for the 2026-11-20 hardware/firmware red team
// (R1-R9 of its harness, RT-xx in its report). Each one reproduced the attack
// against the unfixed runtime and fails there; they pass with the fixes.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "common/coap/coap.h"
#include "firmware/portable/coap_link.h"
#include "firmware/portable/wire.h"
#include "firmware/sim/virtual_board.h"

namespace talos::fw::sim {
namespace {

std::vector<DeviceEvent> of_kind(const std::vector<DeviceEvent>& v, EventKind k) {
  std::vector<DeviceEvent> out;
  std::copy_if(v.begin(), v.end(), std::back_inserter(out),
               [&](const DeviceEvent& e) { return e.kind == k; });
  return out;
}

std::vector<DeviceEvent> p0(const std::vector<DeviceEvent>& v, EventKind k) {
  std::vector<DeviceEvent> out;
  for (const auto& e : of_kind(v, k)) {
    if (e.priority == Priority::kP0Critical) out.push_back(e);
  }
  return out;
}

void warm_up(Node& n) {
  n.run_for(PirDriver::kWarmMinMs + PirDriver::kQuietMs);
  n.drain_and_ack();
}

// R1 / RT-04: one I2C glitch in the middle of a TMAG5273 poll used to leave the
// sensor in standby with its latch cleared: no door event until a reboot.
TEST(RedTeam, R1_TmagGlitchMidPollIsRetriedAndReArmed) {
  Node n(default_board());
  n.insert(0, ModuleCode::kTmag5273Contact);
  n.hall(0).set_field(30'000, 0, 0);  // closed
  n.boot();
  n.run_for(200);
  n.drain_and_ack();
  n.i2c.fail_in = 2;  // the poll's second register write (after the latch-clearing first one)
  n.hall(0).set_field(200, 0, 0);  // the door opens: WAKE latches high
  n.run_for(1'000);
  auto ev = n.drain_and_ack();
  auto opened = p0(ev, EventKind::kContact);
  ASSERT_EQ(opened.size(), 1u) << "the opening behind the glitch must still be reported";
  EXPECT_TRUE(opened[0].flag);
  EXPECT_TRUE(n.hall(0).in_wake_sleep()) << "sensor left unarmed (standby, 0.45 mA)";
  // And it keeps working: close and open again.
  for (int i = 0; i < 3; ++i) {
    n.hall(0).set_field(30'000, 0, 0);
    n.run_for(500);
    n.hall(0).set_field(200, 0, 0);
    n.run_for(500);
  }
  EXPECT_EQ(p0(n.drain_and_ack(), EventKind::kContact).size(), 3u);
}

// R2 / RT-19 and RT-14: intervals are bounded, so environment_interval_ms * 4
// can never wrap and a 49-day heartbeat can never blind supervision.
TEST(RedTeam, R2_ConfigIntervalsHaveUpperBounds) {
  Node n(default_board());
  n.boot();
  n.run_for(50);
  n.drain_and_ack();
  ConfigUpdate c;
  c.revision = 7;
  c.environment_interval_ms = 0x40000000u;  // 12.4 days
  c.heartbeat_interval_ms = 0xFFFFFFF0u;    // 49.7 days
  n.link.downlink.push_back(c);
  n.run_for(50);
  EXPECT_EQ(n.runtime().config().revision, 7u);
  EXPECT_LE(n.runtime().config().environment_interval_ms, NodeRuntime::kMaxEnvironmentIntervalMs);
  EXPECT_LE(n.runtime().config().heartbeat_interval_ms, NodeRuntime::kMaxHeartbeatIntervalMs);
}

// R3 / RT-02: NVS writes per event. The unfixed node rewrote a ~328 B blob on
// every ACK of anything and on every P0 (>= 3 writes per motion cycle, ~60 days
// to wear out the stock 16 KB NVS at continuous motion).
TEST(RedTeam, R3_NvsIsWrittenOncePerAlarmAndNeverPerAck) {
  Node n(default_board());
  n.insert(0, ModuleCode::kPirEkmb);
  n.boot();
  warm_up(n);
  n.run_for(100);
  n.drain_and_ack();
  n.run_for(NodeRuntime::kAckFlushMs + 100);  // let any deferred ACK record land first
  n.drain_and_ack();
  const auto p_idle = n.store.puts;
  for (int i = 0; i < 10; ++i) {  // ten heartbeats, each acknowledged
    n.run_for(60'000);
    n.drain_and_ack();
  }
  const auto idle_puts = n.store.puts - p_idle;
  EXPECT_EQ(idle_puts, 0u) << "heartbeats and their ACKs must not touch NVS";

  // One motion cycle, write by write.
  const auto before = n.store.puts;
  n.gpio.set(0, true);
  n.run_for(2'600);
  EXPECT_EQ(n.store.puts - before, 1u) << "the P0 alarm: exactly one small record";
  n.drain_and_ack();
  n.gpio.set(0, false);
  n.run_for(1'300);
  n.drain_and_ack();
  EXPECT_EQ(n.store.puts - before, 1u) << "its ACK and the P1 clear: no write";

  // 100 cycles: one write per alarm, at most one coalesced ACK record on top.
  const auto p1 = n.store.puts;
  for (int i = 0; i < 100; ++i) {
    n.gpio.set(0, true);
    n.run_for(2'600);
    n.drain_and_ack();
    n.gpio.set(0, false);
    n.run_for(1'300);
    n.drain_and_ack();
  }
  const auto motion_puts = n.store.puts - p1;
  EXPECT_GE(motion_puts, 100u);
  EXPECT_LE(motion_puts, 102u);
  EXPECT_EQ(n.store.puts_by_key[StoreKey::kPendingCritical], 0u);  // the big blob is gone
}

// R4 / RT-03 and RT-15: a jammed link or a dead hub. The unfixed node sent
// ~4 frames/s forever (battery flat in ~3 weeks) and persisted the 8 OLDEST alarms.
TEST(RedTeam, R4_OutageBacksOffToMinutesAndKeepsTheNewestAlarms) {
  Node n(default_board());
  n.insert(0, ModuleCode::kPirEkmb);
  n.boot();
  warm_up(n);
  n.link.connected = false;  // jammed
  for (int i = 0; i < 12; ++i) {
    n.gpio.set(0, true);
    n.run_for(2'600);
    n.gpio.set(0, false);
    n.run_for(1'300);
  }
  n.run_for(10 * 60'000);  // the link has been failing for a while
  const auto a0 = n.link.attempts;
  n.run_for(3'600'000);
  const auto per_hour = n.link.attempts - a0;
  EXPECT_LE(per_hour, 30u) << "a failing link must back off to minutes, not 4/s";
  EXPECT_GE(per_hour, 6u) << "but keep probing so the alarms go out when it heals";

  const auto epoch = n.runtime().boot_epoch();
  n.boot();  // brownout before the link recovers: only persisted alarms survive
  n.link.connected = true;
  n.run_for(1'000);
  std::vector<std::uint32_t> got;
  for (const auto& e : of_kind(n.drain_and_ack(), EventKind::kMotion)) {
    if (e.boot_epoch == epoch && e.flag) got.push_back(e.seq);
  }
  ASSERT_EQ(got.size(), NodeRuntime::kMaxPendingCritical);
  std::sort(got.begin(), got.end());
  // The 12 alarms are the 12 highest motion seqs of that boot; the newest 8 survive.
  EXPECT_GT(got.front(), 8u) << "the oldest alarms were kept instead of the newest";
}

// R5 / RT-14: revision 0xFFFFFFFF used to lock configuration forever.
TEST(RedTeam, R5_MaxConfigRevisionIsRejected) {
  Node n(default_board());
  n.boot();
  n.run_for(50);
  n.drain_and_ack();
  ConfigUpdate bad;
  bad.revision = 0xFFFFFFFFu;
  bad.heartbeat_interval_ms = 0xFFFFFFFFu;
  n.link.downlink.push_back(bad);
  n.run_for(50);
  n.boot();
  ConfigUpdate good;
  good.revision = 2;
  good.heartbeat_interval_ms = 120'000;
  n.link.downlink.push_back(good);
  n.run_for(50);
  EXPECT_EQ(n.runtime().config().revision, 2u);
  EXPECT_EQ(n.runtime().config().heartbeat_interval_ms, 120'000u);
}

// R6 / RT-08: continuous motion from power-up kept the PIR in warm-up forever.
TEST(RedTeam, R6_PirStillReportsMotionThatStartsDuringWarmUp) {
  Node n(default_board());
  n.insert(0, ModuleCode::kPirEkmb);
  n.boot();
  std::vector<DeviceEvent> ev;
  for (int i = 0; i < 600; ++i) {  // 10 min, high 2 s of every 3 s
    n.gpio.set(0, (i % 3) != 2);
    n.run_for(1'000);
    for (const auto& e : n.drain_and_ack()) ev.push_back(e);
  }
  const auto& pir = std::get<PirDriver>(n.runtime().slot(0).driver);
  EXPECT_FALSE(pir.warming());
  EXPECT_FALSE(p0(ev, EventKind::kMotion).empty());
}

// R7 / RT-07: an 8-entry pending ring rejected the hub's ACK as soon as 8
// newer requests had gone out (a Thread SED reply waits for the data poll).
class Wire final : public IDatagram {
 public:
  bool send(const std::uint8_t* d, std::size_t n) override {
    sent.emplace_back(d, d + n);
    return true;
  }
  std::size_t receive(std::uint8_t* buf, std::size_t cap) override {
    if (inbox.empty()) return 0;
    auto f = inbox.front();
    inbox.erase(inbox.begin());
    std::memcpy(buf, f.data(), std::min(cap, f.size()));
    return f.size();
  }
  std::vector<std::vector<std::uint8_t>> sent, inbox;
};

std::vector<std::uint8_t> ack_reply(const std::vector<std::uint8_t>& request, wire::Key key,
                                    std::uint32_t seq) {
  coap::Message req;
  EXPECT_TRUE(coap::parse(request.data(), request.size(), req));
  wire::Tag tag{};
  wire::decode_uplink(req.payload, req.payload_len, key, &tag, [](const DeviceEvent&) {});
  std::uint8_t body[64];
  wire::DownlinkWriter w(body, sizeof body);
  w.add(Ack{1, seq});
  const auto bn = w.finish(key, tag);
  coap::Message rep;
  rep.type = coap::Type::kAck;
  rep.code = coap::kChanged;
  rep.message_id = req.message_id;
  rep.token_len = 4;
  rep.token = req.token;
  rep.payload = body;
  rep.payload_len = bn;
  std::uint8_t out[128];
  const auto on = coap::build(rep, out, sizeof out);
  return {out, out + on};
}

TEST(RedTeam, R7_LateAckAfterABacklogIsStillAccepted) {
  Wire net;
  static const std::uint8_t keyb[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  const wire::Key key{keyb, sizeof keyb};
  CoapLink link(net, "node1", key);
  // A full queue's worth of requests in flight (NodeRuntime::kQueueCapacity).
  for (std::uint32_t i = 1; i <= NodeRuntime::kQueueCapacity; ++i) {
    DeviceEvent e;
    e.boot_epoch = 1;
    e.seq = i;
    ASSERT_TRUE(link.send(e));
  }
  net.inbox.push_back(ack_reply(net.sent[0], key, 1));  // the oldest one, answered last
  const auto d = link.receive();
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(std::get<Ack>(*d).seq, 1u);
  EXPECT_EQ(link.rejected_replies(), 0u);
}

// R8 / RT-01: on the ESP32-H2 a deep-sleep wake is a reset. The unfixed boot
// cut every card's power, turned the opening that woke the node into a P1
// boot report and cleared the impact latch.
TEST(RedTeam, R8_EventThatWokeTheNodeIsReportedAsAnAlarm) {
  Node n(tl_sn_001_board());
  n.insert(0, ModuleCode::kTmag5273Contact);
  n.insert(1, ModuleCode::kTlMod003Imu);
  n.hall(0).set_field(30'000, 0, 0);
  n.boot();
  n.run_for(200);
  n.drain_and_ack();
  const auto cuts0 = n.rev_a().aon_cuts[0];
  const auto cuts1 = n.rev_a().aon_cuts[1];
  // Asleep: the door opens and a window is struck; both sensors latch WAKE.
  n.hall(0).set_field(200, 0, 0);
  n.imu(1).impact(1'500);
  const std::uint32_t lines =
      (1u << n.board.slots[0].wake_line) | (1u << n.board.slots[1].wake_line);
  ASSERT_TRUE(n.gpio.read(n.board.slots[0].wake_line));
  ASSERT_TRUE(n.gpio.read(n.board.slots[1].wake_line));
  n.wake_from_deep_sleep(lines);
  n.run_for(500);
  const auto ev = n.drain_and_ack();
  const auto opened = p0(ev, EventKind::kContact);
  ASSERT_EQ(opened.size(), 1u);
  EXPECT_TRUE(opened[0].flag);
  EXPECT_EQ(opened[0].b, 0u) << "a transition, not a boot state report";
  const auto impacts = p0(ev, EventKind::kImpact);
  ASSERT_EQ(impacts.size(), 1u);
  EXPECT_GE(impacts[0].a, 1'000);
  EXPECT_EQ(n.rev_a().aon_cuts[0], cuts0) << "the wake must not power-cycle the cards";
  EXPECT_EQ(n.rev_a().aon_cuts[1], cuts1);
}

// R9 / RT-09: an abandoned download used to block every later image.
TEST(RedTeam, R9_NewerOfferSupersedesAnAbandonedDownload) {
  Node n(default_board());
  n.boot();
  n.run_for(50);
  n.drain_and_ack();
  OtaOffer a;
  a.version = 2;
  a.size = 4096;
  a.sha256[0] = 0xAA;
  n.link.downlink.push_back(a);
  n.run_for(50);
  OtaChunk c;
  c.offset = 0;
  c.length = 128;
  n.link.downlink.push_back(c);
  n.run_for(50);  // the hub then cancels the campaign
  n.boot();
  n.run_for(3'600'000);
  OtaOffer b;
  b.version = 3;
  b.size = 4096;
  b.sha256[0] = 0xBB;
  n.link.downlink.push_back(b);
  n.run_for(50);
  EXPECT_EQ(n.runtime().ota().state, OtaState::kDownloading);
  EXPECT_EQ(n.runtime().ota().target_version, 3u);
  EXPECT_EQ(n.runtime().ota().received, 0u);
}

}  // namespace
}  // namespace talos::fw::sim
