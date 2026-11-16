#include "simulation/base/geometry.h"

#include <cmath>

namespace talos::sim {

double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
double length(Vec2 v) { return std::sqrt(dot(v, v)); }

namespace {
int orientation(Vec2 a, Vec2 b, Vec2 c) {
  const double v = cross(b - a, c - a);
  if (v > 1e-12) return 1;
  if (v < -1e-12) return -1;
  return 0;
}
bool on_segment(Vec2 a, Vec2 b, Vec2 p) {
  return std::fmin(a.x, b.x) - 1e-12 <= p.x && p.x <= std::fmax(a.x, b.x) + 1e-12 &&
         std::fmin(a.y, b.y) - 1e-12 <= p.y && p.y <= std::fmax(a.y, b.y) + 1e-12;
}
}  // namespace

bool intersects(const Segment& s, const Segment& t) {
  const int o1 = orientation(s.a, s.b, t.a), o2 = orientation(s.a, s.b, t.b);
  const int o3 = orientation(t.a, t.b, s.a), o4 = orientation(t.a, t.b, s.b);
  if (o1 != o2 && o3 != o4) return true;
  if (o1 == 0 && on_segment(s.a, s.b, t.a)) return true;
  if (o2 == 0 && on_segment(s.a, s.b, t.b)) return true;
  if (o3 == 0 && on_segment(t.a, t.b, s.a)) return true;
  if (o4 == 0 && on_segment(t.a, t.b, s.b)) return true;
  return false;
}

bool contains(const std::vector<Vec2>& poly, Vec2 p) {
  bool inside = false;
  for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const Vec2 a = poly[i], b = poly[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
      inside = !inside;
    }
  }
  return inside;
}

bool line_of_sight(Vec2 from, Vec2 to, const std::vector<Segment>& walls) {
  const Segment ray{from, to};
  for (const auto& w : walls) {
    if (intersects(ray, w)) return false;
  }
  return true;
}

Wedge::Wedge(Vec2 apex, double heading_deg, double half_angle_deg, double range_m)
    : apex_(apex), range_(range_m) {
  constexpr double kDeg = 3.14159265358979323846 / 180.0;
  dir_ = {std::cos(heading_deg * kDeg), std::sin(heading_deg * kDeg)};
  cos_half_ = std::cos(half_angle_deg * kDeg);
}

bool Wedge::contains(Vec2 p) const {
  const Vec2 d = p - apex_;
  const double dist = length(d);
  if (dist > range_) return false;
  if (dist < 1e-9) return true;
  return dot(d, dir_) / dist >= cos_half_;
}

}  // namespace talos::sim
