#include "core/state/state.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "core/ingest/ingestor.h"
#include "core/testing/fixtures.h"

namespace talos {
namespace {

using testing::contact;
using testing::motion;

const CapabilityRef kDoor{DeviceId("node-12"), SlotIndex{0}, CapabilityId("contact.open_state")};
const CapabilityRef kYard{DeviceId("node-07"), SlotIndex{0}, CapabilityId("motion.detect")};

class StateTest : public ::testing::Test {
 protected:
  void feed(const ObservationEnvelope& e, std::int64_t at_ms) {
    auto r = ingestor.ingest(e, Timestamp{at_ms});
    ASSERT_NE(r.status, IngestStatus::kRejected) << r.error->message;
  }
  Property property = testing::sample_property();
  InMemoryJournal journal;
  Ingestor ingestor{property, journal};
};

TEST_F(StateTest, ObservationsProjectToCurrentState) {
  feed(contact("node-12", 1, 1, true), 1'000);
  feed(motion("node-07", 1, 1), 2'000);
  feed(contact("node-12", 1, 2, false), 3'000);
  PropertyState s = replay({}, journal);
  EXPECT_EQ(s.through_seq, 3u);
  EXPECT_EQ(std::get<ContactState>(s.capabilities.at(kDoor).value).open, false);
  EXPECT_EQ(std::get<MotionState>(s.capabilities.at(kYard).value).detected, true);
}

TEST_F(StateTest, ReplayIsDeterministic) {
  feed(contact("node-12", 1, 1, true), 1'000);
  feed(motion("node-07", 1, 1), 2'000);
  EXPECT_EQ(replay({}, journal), replay({}, journal));
}

TEST_F(StateTest, SnapshotPlusTailEqualsFullReplay) {
  feed(contact("node-12", 1, 1, true), 1'000);
  feed(motion("node-07", 1, 1), 2'000);
  PropertyState snapshot = replay({}, journal);
  feed(contact("node-12", 1, 2, false), 3'000);
  feed(motion("node-07", 1, 2, false), 4'000);
  EXPECT_EQ(snapshot.through_seq, 2u);
  EXPECT_EQ(replay(snapshot, journal), replay({}, journal));
}

TEST_F(StateTest, ReapplyingAnEntryIsNoOp) {
  feed(contact("node-12", 1, 1, true), 1'000);
  PropertyState s = replay({}, journal);
  auto entries = read_all(journal);
  EXPECT_EQ(reduce(s, entries[0]), s);
}

TEST_F(StateTest, DuplicateDeliveryLeavesStateUnchanged) {
  feed(contact("node-12", 1, 1, true), 1'000);
  PropertyState before = replay({}, journal);
  feed(contact("node-12", 1, 1, true), 1'500);  // retried transport delivery
  EXPECT_EQ(replay({}, journal), before);
}

TEST_F(StateTest, LateObservationDoesNotRegressState) {
  // Device sent open (seq 1) then closed (seq 2); the network reordered them.
  feed(contact("node-12", 1, 2, false), 1'000);
  feed(contact("node-12", 1, 1, true), 1'100);
  PropertyState s = replay({}, journal);
  EXPECT_EQ(journal.last_seq(), 2u);  // both are in history
  EXPECT_EQ(std::get<ContactState>(s.capabilities.at(kDoor).value).open, false);
  EXPECT_EQ(s.capabilities.at(kDoor).position.sequence, 2u);
}

TEST_F(StateTest, NewBootEpochSupersedesOldSequenceNumbers) {
  feed(contact("node-12", 1, 900, true), 1'000);
  feed(contact("node-12", 2, 0, false), 2'000);  // rebooted: seq restarts
  PropertyState s = replay({}, journal);
  EXPECT_EQ(std::get<ContactState>(s.capabilities.at(kDoor).value).open, false);
}

TEST_F(StateTest, StateAtTimeUsesJournalPrefix) {
  feed(contact("node-12", 1, 1, true), 1'000);
  feed(contact("node-12", 1, 2, false), 5'000);
  EXPECT_TRUE(state_at(journal, Timestamp{999}).capabilities.empty());
  EXPECT_EQ(
      std::get<ContactState>(state_at(journal, Timestamp{4'999}).capabilities.at(kDoor).value).open,
      true);
  EXPECT_EQ(
      std::get<ContactState>(state_at(journal, Timestamp{5'000}).capabilities.at(kDoor).value).open,
      false);
}

TEST_F(StateTest, DeviceClockDoesNotAffectOrdering) {
  auto opened = contact("node-12", 1, 1, true);
  opened.observed_at = Timestamp{9'999'999};  // wildly wrong device clock
  auto closed = contact("node-12", 1, 2, false);
  closed.observed_at = Timestamp{1};
  feed(opened, 1'000);
  feed(closed, 2'000);
  EXPECT_EQ(std::get<ContactState>(replay({}, journal).capabilities.at(kDoor).value).open, false);
}

TEST_F(StateTest, FreshnessNeverReportsUnknownAsFresh) {
  PropertyState empty;
  EXPECT_EQ(freshness(empty, kDoor, Timestamp{0}, Millis{60'000}), Freshness::kUnknown);
  feed(contact("node-12", 1, 1, false), 1'000);
  PropertyState s = replay({}, journal);
  EXPECT_EQ(freshness(s, kDoor, Timestamp{61'000}, Millis{60'000}), Freshness::kFresh);
  EXPECT_EQ(freshness(s, kDoor, Timestamp{61'001}, Millis{60'000}), Freshness::kStale);
}

TEST_F(StateTest, CommandTransitionsReplayIntoLedger) {
  CommandTransition created;
  created.command = CommandId("cmd-1");
  created.created = Command{};
  created.created->id = CommandId("cmd-1");
  (void)journal.append(Timestamp{10}, created);
  CommandTransition denied;
  denied.command = CommandId("cmd-1");
  denied.to = CommandStatus::kDenied;
  (void)journal.append(Timestamp{11}, denied);
  EXPECT_EQ(replay({}, journal).commands.find(CommandId("cmd-1"))->status, CommandStatus::kDenied);
}

TEST_F(StateTest, ArmModeIsJournaledState) {
  (void)journal.append(Timestamp{1}, ArmModeChanged{ArmMode::kArmedAway, PrincipalId("u")});
  EXPECT_EQ(replay({}, journal).arm_mode, ArmMode::kArmedAway);
  EXPECT_EQ(PropertyState{}.arm_mode, ArmMode::kDisarmed);
}

// Metamorphic: reordering deliveries across independent devices (each device's
// own order preserved) yields the same final capability state.
TEST(StateMetamorphic, IndependentDeviceInterleavingDoesNotChangeFinalState) {
  Property property = testing::sample_property();
  std::vector<ObservationEnvelope> door, yard;
  for (std::uint64_t i = 1; i <= 20; ++i) {
    door.push_back(contact("node-12", 1, i, i % 2 == 1));
    yard.push_back(motion("node-07", 1, i, i % 3 != 0));
  }
  auto final_state = [&](std::uint32_t seed) {
    std::mt19937 rng(seed);
    InMemoryJournal journal;
    Ingestor ingestor(property, journal);
    std::size_t di = 0, yi = 0;
    std::int64_t t = 0;
    while (di < door.size() || yi < yard.size()) {
      bool pick_door = yi == yard.size() || (di < door.size() && (rng() & 1u));
      (void)ingestor.ingest(pick_door ? door[di++] : yard[yi++], Timestamp{t += 10});
    }
    return replay({}, journal).capabilities;
  };
  auto strip = [](auto caps) {  // journal_seq/received_at legitimately differ
    for (auto& [ref, c] : caps) {
      c.journal_seq = 0;
      c.received_at = Timestamp{};
    }
    return caps;
  };
  const auto reference = strip(final_state(1));
  for (std::uint32_t seed = 2; seed < 50; ++seed) {
    EXPECT_EQ(strip(final_state(seed)), reference) << "seed " << seed;
  }
}

}  // namespace
}  // namespace talos
