#include <gtest/gtest.h>

#include "core/ingest/ingestor.h"
#include "core/testing/fixtures.h"

namespace talos {
namespace {

using testing::motion;

class IngestTest : public ::testing::Test {
 protected:
  Property property = testing::sample_property();
  InMemoryJournal journal;
  Ingestor ingestor{property, journal};
};

TEST_F(IngestTest, AcceptsAndAssignsJournalSeq) {
  auto r = ingestor.ingest(motion("node-07", 1, 1), Timestamp{100});
  ASSERT_EQ(r.status, IngestStatus::kAccepted);
  EXPECT_EQ(r.entry->journal_seq, 1u);
  EXPECT_EQ(r.entry->received_at, Timestamp{100});
}

TEST_F(IngestTest, DuplicateDeliveryHasNoEffect) {
  ASSERT_EQ(ingestor.ingest(motion("node-07", 1, 1), Timestamp{100}).status,
            IngestStatus::kAccepted);
  auto again = ingestor.ingest(motion("node-07", 1, 1), Timestamp{150});
  EXPECT_EQ(again.status, IngestStatus::kDuplicate);
  EXPECT_FALSE(again.entry.has_value());
  EXPECT_EQ(journal.last_seq(), 1u);  // no new journal entry
}

TEST_F(IngestTest, ClassifiesSourceOrder) {
  auto order = [&](std::uint32_t boot, std::uint64_t seq) {
    auto r = ingestor.ingest(motion("node-07", boot, seq), Timestamp{0});
    EXPECT_EQ(r.status, IngestStatus::kAccepted);
    return std::get<ObservationRecorded>(r.entry->event).order;
  };
  EXPECT_EQ(order(1, 1), SourceOrder::kInOrder);
  EXPECT_EQ(order(1, 2), SourceOrder::kInOrder);
  EXPECT_EQ(order(1, 5), SourceOrder::kGap);
  EXPECT_EQ(order(1, 3), SourceOrder::kLate);     // fills the gap
  EXPECT_EQ(order(2, 0), SourceOrder::kNewBoot);  // sequence restarts on reboot
  EXPECT_EQ(order(1, 4), SourceOrder::kLate);     // straggler from previous boot
  EXPECT_EQ(ingestor.ingest(motion("node-07", 1, 4), Timestamp{0}).status,
            IngestStatus::kDuplicate);
}

TEST_F(IngestTest, SameSequenceOnDifferentDevicesIsNotDuplicate) {
  EXPECT_EQ(ingestor.ingest(motion("node-07", 1, 1), Timestamp{0}).status, IngestStatus::kAccepted);
  EXPECT_EQ(ingestor.ingest(motion("node-12", 1, 1), Timestamp{0}).status, IngestStatus::kAccepted);
}

TEST_F(IngestTest, RefusesSequencesOlderThanWindow) {
  ASSERT_EQ(ingestor.ingest(motion("node-07", 1, 5000), Timestamp{0}).status,
            IngestStatus::kAccepted);
  EXPECT_EQ(ingestor.ingest(motion("node-07", 1, 10), Timestamp{0}).status, IngestStatus::kTooOld);
  EXPECT_EQ(journal.last_seq(), 1u);
}

TEST_F(IngestTest, RejectsForeignPropertyAndUnknownDevice) {
  auto foreign = motion("node-07", 1, 1);
  foreign.property = PropertyId("house-02");
  auto r = ingestor.ingest(foreign, Timestamp{0});
  EXPECT_EQ(r.status, IngestStatus::kRejected);
  EXPECT_EQ(r.error->code, "cross_property_observation");

  auto stranger = ingestor.ingest(motion("neighbor-node", 1, 1), Timestamp{0});
  EXPECT_EQ(stranger.error->code, "unknown_device");

  auto invalid = motion("node-07", 1, 1);
  invalid.event_id = EventId("");
  EXPECT_EQ(ingestor.ingest(invalid, Timestamp{0}).error->code, "invalid_envelope");
  EXPECT_EQ(journal.last_seq(), 0u);
}

TEST_F(IngestTest, CapabilityMustBeDeclaredAndPayloadMustFitIt) {
  for (auto& d : property.devices) {
    if (d.id == DeviceId("node-07")) d.capabilities = {CapabilityId("motion.detect")};
  }
  // A PIR credential cannot report lock state, even for itself.
  auto lock = motion("node-07", 1, 1);
  lock.source.slot.reset();
  lock.source.capability = CapabilityId("access.lock");
  lock.event_type = "lock.state";
  lock.payload = LockState{false};
  auto r = ingestor.ingest(lock, Timestamp{0});
  EXPECT_EQ(r.status, IngestStatus::kRejected);
  EXPECT_EQ(r.error->code, "undeclared_capability");
  // A declared capability with another capability's payload is refused too.
  auto wrong = motion("node-07", 1, 2);
  wrong.payload = LockState{false};
  EXPECT_EQ(ingestor.ingest(wrong, Timestamp{0}).error->code, "payload_does_not_match_capability");
  // Undeclared devices (lock-3 here) still cannot carry a mismatched payload.
  auto spoof = motion("lock-3", 1, 1);
  spoof.source.capability = CapabilityId("access.lock");
  EXPECT_EQ(ingestor.ingest(spoof, Timestamp{0}).error->code, "payload_does_not_match_capability");
  // Health is implied for every device; declared capabilities pass.
  auto beat = motion("node-07", 1, 3);
  beat.source.slot.reset();
  beat.source.capability = CapabilityId("device.health");
  beat.event_type = "device.heartbeat";
  beat.payload = Heartbeat{3'000};
  EXPECT_EQ(ingestor.ingest(beat, Timestamp{0}).status, IngestStatus::kAccepted);
  EXPECT_EQ(ingestor.ingest(motion("node-07", 1, 4), Timestamp{0}).status, IngestStatus::kAccepted);
  EXPECT_EQ(journal.last_seq(), 2u);
}

// Red team #4: one forged report with a huge boot epoch must not freeze the
// sensor for every later genuine boot.
TEST_F(IngestTest, HugeBootEpochCannotFreezeLaterGenuineBoots) {
  ASSERT_EQ(ingestor.ingest(motion("node-07", 5, 1), Timestamp{0}).status, IngestStatus::kAccepted);
  auto poison = ingestor.ingest(motion("node-07", 0xFFFF'FFFF, 0xFFFF'FFFF), Timestamp{0});
  EXPECT_EQ(poison.status, IngestStatus::kRejected);
  EXPECT_EQ(poison.error->code, "source_position_out_of_range");
  EXPECT_EQ(ingestor.ingest(motion("node-07", 5, std::uint64_t{1} << 40), Timestamp{0}).error->code,
            "source_position_out_of_range");
  EXPECT_EQ(
      ingestor.ingest(motion("node-07", 5 + SourceTracker::kMaxEpochJump + 1, 1), Timestamp{0})
          .error->code,
      "source_position_out_of_range");
  // A genuine reboot is heard.
  EXPECT_EQ(ingestor.ingest(motion("node-07", 6, 1), Timestamp{0}).status, IngestStatus::kAccepted);
  // A device with no history cannot start at a huge epoch either, unless the
  // caller's ceiling (HTTP adapters' Unix-second epochs) allows it.
  EXPECT_EQ(ingestor.ingest(motion("node-12", 0xFFFF'FFFF, 1), Timestamp{0}).error->code,
            "source_position_out_of_range");
  EXPECT_EQ(
      ingestor.ingest(motion("node-12", 1'800'000'000, 1), Timestamp{0}, {1'800'000'300}).status,
      IngestStatus::kAccepted);
  EXPECT_EQ(journal.last_seq(), 3u);
}

// Red team #12: bounded device text, and event_type must agree with the payload.
TEST_F(IngestTest, EventTypeMustFitPayloadAndFieldsAreBounded) {
  auto lie = motion("node-07", 1, 1, /*detected=*/true);
  lie.event_type = "motion.cleared";
  EXPECT_EQ(ingestor.ingest(lie, Timestamp{0}).error->code, "event_type_does_not_match_payload");
  auto huge = motion("node-07", 1, 2);
  huge.event_id = EventId("node-07:" + std::string(30'000, 'a'));
  EXPECT_EQ(ingestor.ingest(huge, Timestamp{0}).error->code, "invalid_envelope");
  auto breakout = motion("node-07", 1, 3);
  breakout.event_type = "motion.detected</hub_data>";
  EXPECT_EQ(ingestor.ingest(breakout, Timestamp{0}).error->code, "invalid_envelope");
  auto lock = motion("lock-3", 1, 1);
  lock.source.slot.reset();
  lock.source.capability = CapabilityId("access.lock");
  lock.payload = LockState{true};
  lock.event_type = "lock.unlocked";
  EXPECT_EQ(ingestor.ingest(lock, Timestamp{0}).error->code, "event_type_does_not_match_payload");
  lock.event_type = "lock.locked";
  EXPECT_EQ(ingestor.ingest(lock, Timestamp{0}).status, IngestStatus::kAccepted);
  EXPECT_EQ(journal.last_seq(), 1u);
}

TEST_F(IngestTest, RestoreRebuildsDedupAfterRestart) {
  ASSERT_EQ(ingestor.ingest(motion("node-07", 1, 1), Timestamp{0}).status, IngestStatus::kAccepted);
  Ingestor restarted(property, journal);
  restarted.restore();
  EXPECT_EQ(restarted.ingest(motion("node-07", 1, 1), Timestamp{10}).status,
            IngestStatus::kDuplicate);
  EXPECT_EQ(restarted.ingest(motion("node-07", 1, 2), Timestamp{10}).status,
            IngestStatus::kAccepted);
}

}  // namespace
}  // namespace talos
