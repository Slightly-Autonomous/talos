// Battery level for primary 2 x AA lithium (Li-FeS2, Energizer L91 class) on
// TL-SN-002 (ADR 0019). Pure, no I/O.
//
// Li-FeS2 sits on a flat plateau for most of its life and rests near 1.8 V/cell
// even when nearly empty, so the level comes from the pair voltage UNDER LOAD
// (the runtime's lowest reading after a radio burst), never the resting voltage.
// PROVISIONAL curve: read off the L91 datasheet's pulse-discharge curves at room
// temperature for a light pulsed load; cold shifts it down (bench, HD item
// "AA pulse and cold"). Thresholds that act on it live in BoardSupport.
#pragma once

#include <cstddef>
#include <cstdint>

namespace talos::fw {

struct BatteryCurvePoint {
  std::uint32_t pair_mv;
  std::uint8_t percent;
};

// Descending voltage. 2.2 V loaded is the low-battery threshold
// (hardware/results/power-budget-sn2.md); the TPS63900 needs 1.8 V.
inline constexpr BatteryCurvePoint kLiFeS2PairCurve[] = {
    {3300, 100}, {3100, 90}, {2950, 70}, {2850, 50}, {2750, 30},
    {2600, 15},  {2400, 8},  {2200, 3},  {2000, 0},
};

// Remaining capacity estimate, 0-100, from the pair voltage under load.
// 0 mV (unknown, or no cells) gives 0.
constexpr std::uint8_t lifes2_pair_percent(std::uint32_t loaded_mv) {
  constexpr auto n = sizeof kLiFeS2PairCurve / sizeof kLiFeS2PairCurve[0];
  if (loaded_mv >= kLiFeS2PairCurve[0].pair_mv) return kLiFeS2PairCurve[0].percent;
  for (std::size_t i = 1; i < n; ++i) {
    const auto& hi = kLiFeS2PairCurve[i - 1];
    const auto& lo = kLiFeS2PairCurve[i];
    if (loaded_mv >= lo.pair_mv) {
      const std::uint32_t span = hi.pair_mv - lo.pair_mv;
      return static_cast<std::uint8_t>(lo.percent +
                                       (hi.percent - lo.percent) * (loaded_mv - lo.pair_mv) / span);
    }
  }
  return 0;
}

static_assert(lifes2_pair_percent(3600) == 100);
static_assert(lifes2_pair_percent(2200) == 3);
static_assert(lifes2_pair_percent(1500) == 0);

}  // namespace talos::fw
