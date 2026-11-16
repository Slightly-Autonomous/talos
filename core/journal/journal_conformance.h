// Conformance suite every IJournal implementation must pass (testonly).
//
// Usage: define a factory type with
//   static std::unique_ptr<IJournal> make();                 // fresh, empty
//   static std::unique_ptr<IJournal> reopen(IJournal& old);   // same storage, or nullptr
// then INSTANTIATE_TYPED_TEST_SUITE_P(Name, JournalConformance, Factory);
#pragma once

#include <gtest/gtest.h>

#include <memory>

#include "core/journal/journal.h"

namespace talos::testing {

inline ObservationEnvelope conformance_observation(std::uint64_t seq) {
  ObservationEnvelope e;
  e.property = PropertyId("house-01");
  e.source = {DeviceId("node-07"), SlotIndex{0}, CapabilityId("motion.detect")};
  e.event_type = "motion.detected";
  e.event_id = EventId("node-07:1:" + std::to_string(seq));
  e.position = {1, seq};
  e.observed_at = Timestamp{static_cast<std::int64_t>(seq) * 7};
  e.payload = MotionState{true};
  return e;
}

inline std::vector<DomainEvent> conformance_events() {
  CommandTransition created;
  created.command = CommandId("cmd-1");
  created.to = CommandStatus::kRequested;
  created.at = Timestamp{5};
  created.reason = "requested";
  created.created = Command{};
  created.created->id = CommandId("cmd-1");
  created.created->operation = "audio.speak";
  created.created->args = AudioSpeak{"hello", 40};
  CommandTransition verified;
  verified.command = CommandId("cmd-1");
  verified.to = CommandStatus::kVerified;
  verified.late = true;
  verified.evidence = EventId("x");
  return {ObservationRecorded{conformance_observation(1), SourceOrder::kInOrder}, created, verified,
          ArmModeChanged{ArmMode::kArmedAway, PrincipalId("owner")},
          ObservationRecorded{conformance_observation(3), SourceOrder::kGap}};
}

template <typename Factory>
class JournalConformance : public ::testing::Test {};
TYPED_TEST_SUITE_P(JournalConformance);

TYPED_TEST_P(JournalConformance, AssignsSequentialSeqFromOne) {
  auto j = TypeParam::make();
  EXPECT_EQ(j->last_seq(), 0u);
  for (std::uint64_t i = 1; i <= 3; ++i) {
    auto e = j->append(Timestamp{100}, conformance_events()[0]);
    ASSERT_TRUE(e.ok());
    EXPECT_EQ(e.value().journal_seq, i);
  }
  EXPECT_EQ(j->last_seq(), 3u);
}

TYPED_TEST_P(JournalConformance, RoundTripsEveryEventKind) {
  auto j = TypeParam::make();
  std::vector<JournalEntry> written;
  std::int64_t t = 10;
  for (const auto& ev : conformance_events())
    written.push_back(j->append(Timestamp{t += 10}, ev).value());
  EXPECT_EQ(read_all(*j), written);
}

TYPED_TEST_P(JournalConformance, ReceivedAtNonDecreasingWithRegressionFlag) {
  auto j = TypeParam::make();
  (void)j->append(Timestamp{500}, conformance_events()[0]);
  auto back = j->append(Timestamp{200}, conformance_events()[0]).value();
  EXPECT_EQ(back.received_at, Timestamp{500});
  EXPECT_TRUE(back.clock_regressed);
  EXPECT_FALSE(j->append(Timestamp{600}, conformance_events()[0]).value().clock_regressed);
}

TYPED_TEST_P(JournalConformance, PagesAfterSeq) {
  auto j = TypeParam::make();
  for (int i = 0; i < 5; ++i) (void)j->append(Timestamp{i}, conformance_events()[0]);
  auto page = j->read(1, 2);
  ASSERT_EQ(page.size(), 2u);
  EXPECT_EQ(page[0].journal_seq, 2u);
  EXPECT_EQ(page[1].journal_seq, 3u);
  EXPECT_TRUE(j->read(5, 10).empty());
  EXPECT_EQ(read_all(*j, 3).size(), 2u);
}

TYPED_TEST_P(JournalConformance, SurvivesReopen) {
  auto j = TypeParam::make();
  for (const auto& ev : conformance_events()) (void)j->append(Timestamp{50}, ev);
  const auto before = read_all(*j);
  auto reopened = TypeParam::reopen(*j);
  if (!reopened) GTEST_SKIP() << "non-durable implementation";
  j.reset();
  EXPECT_EQ(read_all(*reopened), before);
  EXPECT_EQ(reopened->last_seq(), before.size());
  // Ordering authority continues; received_at stays non-decreasing.
  auto next = reopened->append(Timestamp{10}, conformance_events()[0]).value();
  EXPECT_EQ(next.journal_seq, before.size() + 1);
  EXPECT_EQ(next.received_at, Timestamp{50});
  EXPECT_TRUE(next.clock_regressed);
}

REGISTER_TYPED_TEST_SUITE_P(JournalConformance, AssignsSequentialSeqFromOne,
                            RoundTripsEveryEventKind, ReceivedAtNonDecreasingWithRegressionFlag,
                            PagesAfterSeq, SurvivesReopen);

}  // namespace talos::testing
