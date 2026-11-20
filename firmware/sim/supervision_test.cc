// Supervision, persistence and power hardening from the 2026-11-20 red team
// (RT-01, RT-02, RT-03, RT-05, RT-06, RT-10, RT-11): the parts its harness
// did not reproduce directly.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "common/sha256/sha256.h"
#include "firmware/portable/serial_link.h"
#include "firmware/sim/virtual_board.h"

namespace talos::fw::sim {
namespace {

std::vector<DeviceEvent> run_acked(Node& n, TimeMs ms) {
  std::vector<DeviceEvent> out;
  for (TimeMs t = 0; t < ms; t += 1'000) {
    n.run_for(std::min<TimeMs>(1'000, ms - t));
    for (const auto& e : n.drain_and_ack()) out.push_back(e);
  }
  return out;
}

std::vector<DeviceEvent> p0_faults(const std::vector<DeviceEvent>& v, ModuleFault f) {
  std::vector<DeviceEvent> out;
  for (const auto& e : v) {
    if (e.kind == EventKind::kModuleFault && e.a == static_cast<std::int32_t>(f) &&
        e.priority == Priority::kP0Critical) {
      out.push_back(e);
    }
  }
  return out;
}

std::vector<DeviceEvent> p0_of(const std::vector<DeviceEvent>& v, EventKind k) {
  std::vector<DeviceEvent> out;
  for (const auto& e : v) {
    if (e.kind == k && e.priority == Priority::kP0Critical) out.push_back(e);
  }
  return out;
}

// --- RT-05: sensor loss raises a P0 trouble event ------------------------------------------

TEST(Supervision, PulledCardIsAP0TamperFaultOnBothBoardKinds) {
  for (const bool rev_a : {true, false}) {
    Node n(rev_a ? tl_sn_001_board() : default_board());
    n.insert(0, ModuleCode::kTmag5273Contact);
    n.hall(0).set_field(30'000, 0, 0);
    n.boot();
    run_acked(n, 2'000);
    n.remove(0);  // an intruder opens the enclosure and pulls the door card
    const auto ev = run_acked(n, 61'000);
    const auto removed = p0_faults(ev, ModuleFault::kCardRemoved);
    ASSERT_EQ(removed.size(), 1u) << (rev_a ? "TL-SN-001 (PRESENT_N)" : "no presence line (ID)");
    EXPECT_EQ(removed[0].slot, 0);
    EXPECT_EQ(removed[0].b, static_cast<std::uint32_t>(ModuleCode::kTmag5273Contact));
    EXPECT_FALSE(n.runtime().slot(0).enabled);
    if (rev_a) EXPECT_FALSE(n.rev_a().aon_on(0));  // nothing left drawing from AON
    // Put back: discovered again at the next heartbeat.
    n.insert(0, ModuleCode::kTmag5273Contact);
    n.hall(0).set_field(30'000, 0, 0);
    run_acked(n, 61'000);
    EXPECT_TRUE(n.runtime().slot(0).enabled);
  }
}

TEST(Supervision, OneMissedIdReadIsNotARemoval) {
  Node n(default_board());
  n.insert(0, ModuleCode::kTmag5273Contact);
  n.hall(0).set_field(30'000, 0, 0);
  n.boot();
  run_acked(n, 59'990);
  n.i2c.fail_in = 1;  // the ID read during the heartbeat check glitches once
  const auto ev = run_acked(n, 2'000);
  EXPECT_TRUE(p0_faults(ev, ModuleFault::kCardRemoved).empty());
  EXPECT_TRUE(n.runtime().slot(0).enabled);
}

TEST(Supervision, QuarantinedCriticalSlotIsP0) {
  Node n(tl_sn_001_board());
  n.insert(1, ModuleCode::kTlMod003Imu);
  const std::array<std::uint8_t, 6> rec = {1, 0, 0, 1, 0, 0};  // powering slot 1, strike 1
  ASSERT_TRUE(n.store.put(StoreKey::kSlotPower, rec.data(), rec.size()));
  n.boot();  // second reset while powering slot 1: quarantined
  const auto ev = run_acked(n, 1'000);
  ASSERT_EQ(p0_faults(ev, ModuleFault::kPowerQuarantined).size(), 1u);
}

TEST(Supervision, StuckHighWakeIsMaskedAsAWakeSourceAndRaisedAsP0) {
  Node n(default_board());
  n.insert(1, ModuleCode::kLis2dw12);
  n.boot();
  EXPECT_TRUE(n.sleep.wake_enabled(1));
  n.imu(1).int_stuck_high = true;
  n.gpio.set(1, true);
  auto ev = run_acked(n, 1'000);
  EXPECT_EQ(p0_faults(ev, ModuleFault::kStuckInterrupt).size(), 1u);
  run_acked(n, NodeRuntime::kWakeMaskMs);
  EXPECT_FALSE(n.sleep.wake_enabled(1)) << "a stuck line would re-wake the node forever";
  EXPECT_NE(n.runtime().wake_masked_slots() & 0x2, 0);
  n.imu(1).int_stuck_high = false;  // the bridge clears
  n.gpio.set(1, false);
  run_acked(n, 2 * NodeRuntime::kMaskedPollMs);
  EXPECT_TRUE(n.sleep.wake_enabled(1));
  // And impacts are seen again.
  n.imu(1).impact(900);
  EXPECT_EQ(p0_of(run_acked(n, 500), EventKind::kImpact).size(), 1u);
}

TEST(Supervision, PirHeldHighIsMaskedButNotAFaultUntilMinutesPass) {
  Node n(default_board());
  n.insert(0, ModuleCode::kPirEkmb);
  n.boot();
  run_acked(n, PirDriver::kWarmMinMs + PirDriver::kQuietMs);
  n.gpio.set(0, true);  // someone moving continuously, or a stuck output
  auto ev = run_acked(n, NodeRuntime::kWakeMaskMs + 2'000);
  EXPECT_EQ(p0_of(ev, EventKind::kMotion).size(), 1u);
  EXPECT_FALSE(n.sleep.wake_enabled(0));
  EXPECT_TRUE(p0_faults(ev, ModuleFault::kStuckInterrupt).empty());
  ev = run_acked(n, NodeRuntime::kWakeStuckFaultMs);
  EXPECT_EQ(p0_faults(ev, ModuleFault::kStuckInterrupt).size(), 1u);
}

TEST(Supervision, TmagWhoseWakeNeverRisesIsFoundAtTheHeartbeat) {
  Node n(default_board());
  n.insert(0, ModuleCode::kTmag5273Contact);
  n.hall(0).set_field(30'000, 0, 0);
  n.boot();
  run_acked(n, 1'000);
  n.gpio.force_low[0] = true;      // TVS failed short / contact 9 open
  n.hall(0).set_field(200, 0, 0);  // the door opens: the sensor latches, the MCU never sees it
  const auto ev = run_acked(n, 61'000);
  EXPECT_EQ(p0_faults(ev, ModuleFault::kWakeLineFault).size(), 1u);
  const auto opened = p0_of(ev, EventKind::kContact);
  ASSERT_EQ(opened.size(), 1u);  // late, but not lost
  EXPECT_TRUE(opened[0].flag);
  EXPECT_TRUE(n.hall(0).in_wake_sleep());
}

TEST(Supervision, ImuWhoseWakeNeverRisesIsFoundAtTheHeartbeat) {
  Node n(default_board());
  n.insert(1, ModuleCode::kLis2dw12);
  n.boot();
  run_acked(n, 1'000);
  n.gpio.force_low[1] = true;
  n.imu(1).impact(1'200);
  const auto ev = run_acked(n, 61'000);
  EXPECT_EQ(p0_faults(ev, ModuleFault::kWakeLineFault).size(), 1u);
  EXPECT_EQ(p0_of(ev, EventKind::kImpact).size(), 1u);
}

// --- RT-04: bounded re-arm, then low power and a P0 ----------------------------------------

TEST(Supervision, TmagThatCannotBeReArmedIsParkedInSleepAndRaisedAsP0) {
  Node n(default_board());
  n.insert(0, ModuleCode::kTmag5273Contact);
  n.hall(0).set_field(30'000, 0, 0);
  n.boot();
  run_acked(n, 1'000);
  n.hall(0).never_converts = true;  // every conversion times out from now on
  n.hall(0).set_field(200, 0, 0);
  const auto ev =
      run_acked(n, Tmag5273ContactDriver::kRetryMs * (Tmag5273ContactDriver::kMaxRetries + 2));
  const auto bus = p0_faults(ev, ModuleFault::kBusError);
  ASSERT_EQ(bus.size(), 1u);
  EXPECT_EQ(bus[0].b, kFaultDetailUnarmed);
  EXPECT_EQ(n.hall(0).mode(), Tmag5273ContactDriver::kModeSleep);  // 5 nA, not 0.45 mA standby
  std::size_t health = 0;
  for (const auto& e : ev) {
    health += e.a == static_cast<std::int32_t>(ModuleFault::kBusError) &&
                      e.priority == Priority::kP2Health
                  ? 1
                  : 0;
  }
  EXPECT_EQ(health, 1u) << "one health report per episode, not one per retry";
  n.hall(0).never_converts = false;  // heals: the slow retry re-arms it and reports the opening
  const auto later = run_acked(n, Tmag5273ContactDriver::kSlowRetryMs + 1'000);
  EXPECT_EQ(p0_of(later, EventKind::kContact).size(), 1u);
  EXPECT_TRUE(n.hall(0).in_wake_sleep());
}

// --- RT-06: the epoch and identity survive an erase of the working NVS ---------------------

TEST(Persistence, EpochAndIdentitySurviveAnEraseOfTheWorkingPartition) {
  Node n(default_board());
  DeviceIdentity id;
  std::strcpy(id.device_id, "node-7");
  id.key_len = 16;
  for (int i = 0; i < 16; ++i) id.key[i] = static_cast<std::uint8_t>(i + 1);
  ASSERT_TRUE(save_identity(n.store, id));
  n.boot();
  n.boot();
  const auto e = n.runtime().boot_epoch();
  n.store.erase_working_partition();  // NO_FREE_PAGES, or a rollback across an NVS format change
  n.boot();
  EXPECT_EQ(n.runtime().boot_epoch(), e + 1) << "an epoch at 1 is too_old to the hub";
  const auto back = load_identity(n.store);
  ASSERT_TRUE(back);
  EXPECT_STREQ(back->device_id, "node-7");
}

// --- RT-01: deep-sleep wakes ---------------------------------------------------------------

TEST(DeepSleep, PirStaysPoweredAndWarmAcrossAWake) {
  Node n(tl_sn_001_board());
  n.insert(2, ModuleCode::kTlMod002Pir);
  n.boot();
  run_acked(n, PirDriver::kWarmMinMs + PirDriver::kQuietMs);
  ASSERT_FALSE(std::get<PirDriver>(n.runtime().slot(2).driver).warming());
  const auto cuts = n.rev_a().aon_cuts[2];
  const std::uint8_t line = n.board.slots[2].wake_line;
  n.gpio.set(line, true);  // motion wakes the node
  n.wake_from_deep_sleep(1u << line);
  const auto ev = run_acked(n, 100);
  EXPECT_EQ(p0_of(ev, EventKind::kMotion).size(), 1u) << "no 30 s warm-up after a wake";
  EXPECT_EQ(n.rev_a().aon_cuts[2], cuts);
  EXPECT_TRUE(n.runtime().resumed_from_sleep());
}

TEST(DeepSleep, AWakeWritesNoNvsAndNeverReusesASequenceNumber) {
  Node n(tl_sn_001_board());
  n.insert(0, ModuleCode::kTmag5273Contact);
  n.hall(0).set_field(30'000, 0, 0);
  n.boot();
  std::uint32_t max_seq = 0;
  for (const auto& e : run_acked(n, 1'000)) max_seq = std::max(max_seq, e.seq);
  const auto epoch = n.runtime().boot_epoch();
  const auto puts = n.store.puts;
  n.hall(0).set_field(200, 0, 0);
  n.wake_from_deep_sleep(1u << n.board.slots[0].wake_line);
  EXPECT_EQ(n.runtime().boot_epoch(), epoch);
  const auto ev = run_acked(n, 500);
  ASSERT_FALSE(ev.empty());
  for (const auto& e : ev) {
    EXPECT_EQ(e.boot_epoch, epoch);
    EXPECT_GT(e.seq, max_seq);
  }
  EXPECT_EQ(n.store.puts - puts, 1u) << "only the P0 opening's record, no epoch, no slot marker";
  n.boot();  // a real reset afterwards still moves the epoch on
  EXPECT_EQ(n.runtime().boot_epoch(), epoch + 1);
}

TEST(DeepSleep, AcknowledgedAlarmsAreNotResentAfterAWake) {
  Node n(tl_sn_001_board());
  n.insert(0, ModuleCode::kTmag5273Contact);
  n.hall(0).set_field(30'000, 0, 0);
  n.boot();
  run_acked(n, 1'000);
  n.hall(0).set_field(200, 0, 0);  // P0, persisted, then acknowledged (NVS not yet updated)
  run_acked(n, 1'000);
  n.hall(0).set_field(30'000, 0, 0);
  run_acked(n, 1'000);
  n.wake_from_deep_sleep(0);  // timer wake
  const auto ev = run_acked(n, 1'000);
  EXPECT_TRUE(p0_of(ev, EventKind::kContact).empty()) << "LP RAM carries the ACKs across the wake";
}

TEST(DeepSleep, ColdBootIgnoresStaleRetainedState) {
  Node n(tl_sn_001_board());
  n.insert(0, ModuleCode::kTmag5273Contact);
  n.hall(0).set_field(30'000, 0, 0);
  n.boot();
  run_acked(n, 1'000);
  const auto epoch = n.runtime().boot_epoch();
  n.boot();  // software reset: LP RAM still holds a valid block, but this is no wake
  EXPECT_EQ(n.runtime().boot_epoch(), epoch + 1);
  EXPECT_FALSE(n.runtime().resumed_from_sleep());
}

// --- RT-03: outage rationing, TTL, coalescing ----------------------------------------------

TEST(Delivery, AfterAnOutageOnlyFreshTelemetryAndOneHeartbeatGoOut) {
  Node n(default_board());
  n.insert(0, ModuleCode::kPirEkmb);
  n.boot();
  run_acked(n, 35'000);
  n.link.connected = false;
  n.run_for(60 * 60'000);
  EXPECT_TRUE(n.runtime().link_failing());
  n.link.connected = true;
  const TimeMs back = n.clock.now_ms();
  const auto ev = run_acked(n, NodeRuntime::kProbeMaxMs + 5'000);
  EXPECT_FALSE(n.runtime().link_failing());
  std::size_t heartbeats_from_outage = 0;
  for (const auto& e : ev) {
    // Telemetry from more than one TTL before the link came back has expired.
    if (e.priority >= Priority::kP2Health) {
      EXPECT_GT(e.at_ms + NodeRuntime::kTelemetryTtlMs, back) << "stale telemetry resent";
    }
    if (e.kind == EventKind::kHeartbeat && e.at_ms < back) ++heartbeats_from_outage;
  }
  EXPECT_LE(heartbeats_from_outage, 1u);  // coalesced: only the newest
  EXPECT_GT(n.runtime().expired_events(), 0u);
}

TEST(Delivery, NewAlarmDuringAnOutageGetsAProbeWithinThirtySeconds) {
  Node n(default_board());
  n.insert(0, ModuleCode::kPirEkmb);
  n.boot();
  run_acked(n, 35'000);
  n.link.connected = false;
  n.run_for(20 * 60'000);  // deep in the backoff: probes are minutes apart
  ASSERT_TRUE(n.runtime().link_failing());
  n.link.connected = true;  // the hub is back, the node does not know yet
  n.gpio.set(0, true);
  n.run_for(NodeRuntime::kP0ProbeMinMs + 100);
  const auto motion = p0_of(n.drain_and_ack(), EventKind::kMotion);
  EXPECT_EQ(motion.size(), 1u);
}

TEST(Delivery, DownlinkFloodIsHandledInBoundedSteps) {
  Node n(default_board());
  n.boot();
  n.run_for(10);
  for (int i = 0; i < 100; ++i) n.link.downlink.push_back(Ack{1, 999});
  n.runtime().step();
  EXPECT_EQ(n.link.downlink.size(), 100u - NodeRuntime::kMaxDownlinksPerStep);
  EXPECT_EQ(n.runtime().next_wake_ms(), n.clock.now_ms());  // more to do right away
  n.run_for(20);
  EXPECT_TRUE(n.link.downlink.empty());
}

// --- RT-10: watchdog through a slot erase -------------------------------------------------

TEST(Ota, SlotEraseFeedsTheWatchdogBlockByBlock) {
  Node n(tl_sn_001_board(), 1024 * 1024);
  n.flash.erase_ms_per_block = 45;  // 4 KiB sector erase typ: 256 blocks = 11.5 s > 5 s watchdog
  n.esp_polling = true;
  n.boot();
  n.run_for(50);
  OtaOffer o;
  o.version = 2;
  o.size = 1024 * 1024;
  o.sha256[0] = 1;
  n.link.downlink.push_back(o);
  n.run_for(100);
  EXPECT_EQ(n.runtime().ota().state, OtaState::kDownloading);
  EXPECT_LT(n.watchdog.max_gap_ms, n.watchdog.timeout_ms);
  EXPECT_EQ(n.watchdog.bites, 0u);
  EXPECT_EQ(n.reboots(), 0u);
}

TEST(Ota, StalledDownloadIsReplacedEvenBySameVersion) {
  Node n(default_board());
  n.boot();
  OtaOffer a;
  a.version = 2;
  a.size = 4096;
  a.sha256[0] = 0xAA;
  n.link.downlink.push_back(a);
  n.run_for(10);
  OtaOffer rebuilt = a;
  rebuilt.sha256[0] = 0xAB;  // the hub rebuilt v2
  n.link.downlink.push_back(rebuilt);
  n.run_for(10);
  EXPECT_EQ(n.runtime().ota().sha256[0], 0xAA) << "an active download is not replaced";
  n.run_for(Ota::kStallMs);
  n.link.downlink.push_back(rebuilt);
  n.run_for(10);
  EXPECT_EQ(n.runtime().ota().sha256[0], 0xAB);
  EXPECT_EQ(n.runtime().ota().state, OtaState::kDownloading);
}

}  // namespace
}  // namespace talos::fw::sim
