// 2D geometry for the twin (meters). Uses only +,-,*,/ and sqrt on doubles,
// which are IEEE-exact across platforms; trig is evaluated once at setup.
#pragma once

#include <vector>

namespace talos::sim {

struct Vec2 {
  double x = 0;
  double y = 0;
  Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
  Vec2 operator*(double k) const { return {x * k, y * k}; }
  bool operator==(const Vec2&) const = default;
};

double dot(Vec2 a, Vec2 b);
double cross(Vec2 a, Vec2 b);
double length(Vec2 v);

struct Segment {
  Vec2 a;
  Vec2 b;
};

// Proper or touching intersection of two segments.
bool intersects(const Segment& s, const Segment& t);

// Even-odd point-in-polygon; points on an edge may go either way.
bool contains(const std::vector<Vec2>& polygon, Vec2 p);

// True when no wall crosses the open segment from `from` to `to`.
bool line_of_sight(Vec2 from, Vec2 to, const std::vector<Segment>& walls);

// A field-of-view sector: apex, facing direction, half-angle and range.
class Wedge {
 public:
  Wedge(Vec2 apex, double heading_deg, double half_angle_deg, double range_m);
  bool contains(Vec2 p) const;
  Vec2 apex() const { return apex_; }

 private:
  Vec2 apex_;
  Vec2 dir_;
  double cos_half_;
  double range_;
};

}  // namespace talos::sim
