// Tamper detectors beyond the enclosure loop (TL-SN-002, owner 2026-11-22).
// Pure integer logic, no I/O: the runtime feeds them sensor readings it already
// takes and emits EventKind::kTamper with the matching TamperCause.
//
// - MagnetDefeat (TamperCause::kMagnetDefeat, HD-045): while the door reads
//   closed, the 3-axis field must stay near the closed-door baseline captured
//   when it closed. A foreign magnet held at the sensor to fake "closed" shows
//   as a field far stronger than the door magnet's, or pointing elsewhere.
// - MountPose (TamperCause::kMountMoved): the accelerometer's gravity vector
//   must stay near the installed pose, captured when the enclosure is closed
//   after installation. Prying the node off the frame tilts it.
//
// Thresholds are PROVISIONAL until bench C1-06 and the mount bench (HD-049).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <optional>

namespace talos::fw {

struct Vec3 {
  std::int32_t x = 0, y = 0, z = 0;
  bool operator==(const Vec3&) const = default;
};

namespace tamper_math {
inline std::int64_t dot(const Vec3& a, const Vec3& b) {
  return static_cast<std::int64_t>(a.x) * b.x + static_cast<std::int64_t>(a.y) * b.y +
         static_cast<std::int64_t>(a.z) * b.z;
}
inline std::int64_t norm2(const Vec3& a) { return dot(a, a); }
// True if the angle between a and b exceeds the one whose cosine is
// cos_permille / 1000. Squared, so no square root: cos(t) < c <=> for dot >= 0,
// dot^2 < c^2 |a|^2 |b|^2; a negative dot is always past 90 degrees.
inline bool angle_exceeds(const Vec3& a, const Vec3& b, std::int64_t cos_permille) {
  const std::int64_t d = dot(a, b);
  if (d <= 0) return true;
  // In double: the squares overflow 64 bits for fields of ~1e5. Rarely called
  // (a door close, a heartbeat), so soft-float on the ESP32-H2 costs nothing.
  const double lhs = static_cast<double>(d) * static_cast<double>(d) * 1'000'000.0;
  const double rhs = static_cast<double>(cos_permille) * static_cast<double>(cos_permille) *
                     static_cast<double>(norm2(a)) * static_cast<double>(norm2(b));
  return lhs < rhs;
}
}  // namespace tamper_math

class MagnetDefeat {
 public:
  // Field in microtesla. Defeat when, with the door closed:
  //   |B| > kCeilingUt (no door magnet at the specified gap comes close), or
  //   |B| > kRatio x the baseline, or
  //   the direction is more than ~35 degrees from the baseline (cos 0.82).
  // Restored when back inside 1.5x and ~20 degrees (cos 0.94).
  static constexpr std::int64_t kCeilingUt = 100'000;
  static constexpr std::int64_t kRatio = 3;
  static constexpr std::int64_t kCosDefeat = 820;
  static constexpr std::int64_t kCosRestore = 940;

  // The door just closed (or was found closed at boot): this reading is the baseline.
  void set_baseline(const Vec3& b) {
    baseline_ = b;
    defeated_ = false;
  }
  void clear_baseline() { baseline_.reset(); }
  const std::optional<Vec3>& baseline() const { return baseline_; }
  bool defeated() const { return defeated_; }
  // Restores state across a deep-sleep wake (LP RAM).
  void restore(const std::optional<Vec3>& baseline, bool defeated) {
    baseline_ = baseline;
    defeated_ = baseline.has_value() && defeated;
  }

  // A reading taken while the door reads closed. Returns the new state when it
  // changes (true = defeat detected, false = restored), nullopt otherwise.
  std::optional<bool> check(const Vec3& b) {
    if (!baseline_) return std::nullopt;
    const std::int64_t n2 = tamper_math::norm2(b);
    const std::int64_t base2 = tamper_math::norm2(*baseline_);
    bool bad;
    if (!defeated_) {
      bad = n2 > kCeilingUt * kCeilingUt || n2 > kRatio * kRatio * base2 ||
            tamper_math::angle_exceeds(b, *baseline_, kCosDefeat);
    } else {
      // Hysteresis: 1.5x is 9/4 in squares.
      bad = n2 > kCeilingUt * kCeilingUt || 4 * n2 > 9 * base2 ||
            tamper_math::angle_exceeds(b, *baseline_, kCosRestore);
    }
    if (bad == defeated_) return std::nullopt;
    defeated_ = bad;
    return bad;
  }

 private:
  std::optional<Vec3> baseline_;
  bool defeated_ = false;
};

class MountPose {
 public:
  // Gravity in mg. Moved past ~20 degrees (cos 0.94), restored inside ~10 (cos 0.985).
  // Readings far from 1 g (being shaken, carried) are not judged at all.
  static constexpr std::int64_t kCosMoved = 940;
  static constexpr std::int64_t kCosRestore = 985;
  static constexpr std::int64_t kMinMg = 700, kMaxMg = 1300;

  void set_baseline(const Vec3& g) {
    baseline_ = g;
    moved_ = false;
  }
  const std::optional<Vec3>& baseline() const { return baseline_; }
  bool moved() const { return moved_; }
  void restore(const std::optional<Vec3>& baseline, bool moved) {
    baseline_ = baseline;
    moved_ = baseline.has_value() && moved;
  }
  static bool plausible(const Vec3& g) {
    const std::int64_t n2 = tamper_math::norm2(g);
    return n2 >= kMinMg * kMinMg && n2 <= kMaxMg * kMaxMg;
  }
  // Returns the new state when it changes (true = moved, false = back in place).
  std::optional<bool> check(const Vec3& g) {
    if (!baseline_ || !plausible(g)) return std::nullopt;
    const bool bad = tamper_math::angle_exceeds(g, *baseline_, moved_ ? kCosRestore : kCosMoved);
    if (bad == moved_) return std::nullopt;
    moved_ = bad;
    return bad;
  }

 private:
  std::optional<Vec3> baseline_;
  bool moved_ = false;
};

}  // namespace talos::fw
