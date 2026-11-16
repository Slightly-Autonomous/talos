// Deterministic, portable pseudo-random numbers for the twin.
//
// std:: distributions are implementation-defined and differ between libc++
// and libstdc++, which would make a seed reproduce differently on macOS and
// Linux. SplitMix64 plus explicit conversions is identical everywhere.
#pragma once

#include <cstdint>
#include <string_view>

namespace talos::sim {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}

  // Independent stream for a named component, so adding a device does not
  // perturb the randomness of the others.
  static Rng stream(std::uint64_t seed, std::string_view name) {
    std::uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (char c : name) {
      h ^= static_cast<unsigned char>(c);
      h *= 1099511628211ull;
    }
    return Rng(seed ^ h);
  }

  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  // Uniform in [0, 1) with 53 bits of precision.
  double uniform() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

  bool chance(double p) { return p > 0.0 && uniform() < p; }

  // Uniform integer in [lo, hi].
  std::int64_t between(std::int64_t lo, std::int64_t hi) {
    if (hi <= lo) return lo;
    const auto span = static_cast<std::uint64_t>(hi - lo) + 1;
    return lo + static_cast<std::int64_t>(next() % span);
  }

 private:
  std::uint64_t state_;
};

}  // namespace talos::sim
