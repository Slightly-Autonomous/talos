#include <gtest/gtest.h>

#include "simulation/base/geometry.h"
#include "simulation/base/rng.h"

namespace talos::sim {
namespace {

TEST(Rng, KnownSequenceIsPlatformIndependent) {
  // Golden values: must be identical on every compiler and standard library.
  Rng r(42);
  EXPECT_EQ(r.next(), 13679457532755275413ull);
  EXPECT_EQ(r.next(), 2949826092126892291ull);
}

TEST(Rng, StreamsAreIndependentAndRepeatable) {
  Rng a = Rng::stream(7, "node-07"), b = Rng::stream(7, "node-07"), c = Rng::stream(7, "x");
  const auto va = a.next();
  EXPECT_EQ(va, b.next());
  EXPECT_NE(va, c.next());
}

TEST(Rng, UniformAndChanceBounds) {
  Rng r(1);
  for (int i = 0; i < 1000; ++i) {
    const double u = r.uniform();
    EXPECT_GE(u, 0.0);
    EXPECT_LT(u, 1.0);
    const auto k = r.between(-3, 3);
    EXPECT_GE(k, -3);
    EXPECT_LE(k, 3);
  }
  EXPECT_FALSE(r.chance(0.0));
  EXPECT_TRUE(r.chance(1.0));
}

TEST(Geometry, PolygonContainment) {
  std::vector<Vec2> square{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
  EXPECT_TRUE(contains(square, {5, 5}));
  EXPECT_FALSE(contains(square, {15, 5}));
}

TEST(Geometry, WallsBlockLineOfSight) {
  std::vector<Segment> walls{{{5, -1}, {5, 1}}};
  EXPECT_FALSE(line_of_sight({0, 0}, {10, 0}, walls));
  EXPECT_TRUE(line_of_sight({0, 5}, {10, 5}, walls));
}

TEST(Geometry, WedgeCoverage) {
  Wedge w({0, 0}, 0.0, 45.0, 10.0);  // facing +x, 90 degree cone
  EXPECT_TRUE(w.contains({5, 0}));
  EXPECT_TRUE(w.contains({5, 4.9}));
  EXPECT_FALSE(w.contains({5, 5.1}));
  EXPECT_FALSE(w.contains({-1, 0}));
  EXPECT_FALSE(w.contains({11, 0}));
}

}  // namespace
}  // namespace talos::sim
