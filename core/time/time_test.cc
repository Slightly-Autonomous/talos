#include "core/time/time.h"

#include <gtest/gtest.h>

namespace talos {
namespace {

TEST(VirtualClock, AdvancesOnlyWhenTold) {
  VirtualClock clock(Timestamp{1000});
  EXPECT_EQ(clock.now(), Timestamp{1000});
  clock.advance(Millis{250});
  EXPECT_EQ(clock.now(), Timestamp{1250});
  clock.set(Timestamp{5});
  EXPECT_EQ(clock.now(), Timestamp{5});
}

TEST(Timestamp, Arithmetic) {
  Timestamp t{10'000};
  EXPECT_EQ((t + Millis{500}).unix_ms, 10'500);
  EXPECT_EQ((t - Timestamp{4'000}).count, 6'000);
  EXPECT_LT(Timestamp{1}, Timestamp{2});
}

}  // namespace
}  // namespace talos
