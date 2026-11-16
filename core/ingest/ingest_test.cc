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
