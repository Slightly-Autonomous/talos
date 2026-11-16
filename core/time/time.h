// Time primitives for the deterministic core.
//
// Core code never reads the system clock. Hub time is injected via IClock so
// that tests, simulation and replay are reproducible.
#pragma once

#include <compare>
#include <cstdint>

namespace talos {

// Duration in milliseconds.
struct Millis {
  std::int64_t count = 0;
  auto operator<=>(const Millis&) const = default;
};

// A UTC instant in milliseconds since the Unix epoch.
struct Timestamp {
  std::int64_t unix_ms = 0;

  auto operator<=>(const Timestamp&) const = default;
  Timestamp operator+(Millis d) const { return {unix_ms + d.count}; }
  Timestamp operator-(Millis d) const { return {unix_ms - d.count}; }
  Millis operator-(Timestamp other) const { return {unix_ms - other.unix_ms}; }
};

// Source of hub time.
class IClock {
 public:
  virtual ~IClock() = default;
  virtual Timestamp now() const = 0;
};

// Manually advanced clock for tests and the digital twin.
class VirtualClock final : public IClock {
 public:
  explicit VirtualClock(Timestamp start) : now_(start) {}
  Timestamp now() const override { return now_; }
  void advance(Millis d) { now_ = now_ + d; }
  void set(Timestamp t) { now_ = t; }

 private:
  Timestamp now_;
};

}  // namespace talos
