#include "core/journal/journal.h"

#include <gtest/gtest.h>

namespace talos {
namespace {

DomainEvent transition(std::string id) {
  CommandTransition t;
  t.command = CommandId(std::move(id));
  return t;
}

TEST(InMemoryJournal, AssignsStrictlyIncreasingSeq) {
  InMemoryJournal j;
  EXPECT_EQ(j.last_seq(), 0u);
  auto a = j.append(Timestamp{100}, transition("a"));
  auto b = j.append(Timestamp{100}, transition("b"));
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(a.value().journal_seq, 1u);
  EXPECT_EQ(b.value().journal_seq, 2u);
  EXPECT_EQ(j.last_seq(), 2u);
}

TEST(InMemoryJournal, ReceivedAtIsNonDecreasingAndRegressionIsFlagged) {
  InMemoryJournal j;
  (void)j.append(Timestamp{500}, transition("a"));
  auto back = j.append(Timestamp{200}, transition("b"));  // hub clock stepped back
  EXPECT_EQ(back.value().received_at, Timestamp{500});
  EXPECT_TRUE(back.value().clock_regressed);
  auto fwd = j.append(Timestamp{600}, transition("c"));
  EXPECT_FALSE(fwd.value().clock_regressed);
}

TEST(InMemoryJournal, ReadPagesAfterSeq) {
  InMemoryJournal j;
  for (int i = 0; i < 5; ++i) (void)j.append(Timestamp{i}, transition(std::to_string(i)));
  auto page = j.read(1, 2);
  ASSERT_EQ(page.size(), 2u);
  EXPECT_EQ(page[0].journal_seq, 2u);
  EXPECT_EQ(page[1].journal_seq, 3u);
  EXPECT_TRUE(j.read(5, 10).empty());
  EXPECT_EQ(read_all(j, 3).size(), 2u);
}

}  // namespace
}  // namespace talos
