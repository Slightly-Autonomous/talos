// Per-device source-position tracking for deduplication (ADR 0008).
//
// Memory is bounded: for each (device, boot_epoch) we keep the highest
// sequence seen plus a sliding window of which recent sequences arrived. A
// sequence older than the window cannot be proven unique and is refused
// (`kTooOld`) rather than risk a duplicate effect. Only the newest
// kMaxBootsPerDevice boot epochs are remembered per device.
#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>

#include "core/domain/ids.h"
#include "core/events/observation.h"
#include "core/journal/journal.h"

namespace talos {

class SourceTracker {
 public:
  static constexpr std::size_t kWindow = 1024;
  static constexpr std::size_t kMaxBootsPerDevice = 8;

  enum class Verdict : std::uint8_t { kNew, kDuplicate, kTooOld };
  struct Check {
    Verdict verdict = Verdict::kNew;
    SourceOrder order = SourceOrder::kInOrder;  // meaningful when kNew
  };

  Check check(const DeviceId& device, SourcePosition pos) const;
  // Records a position previously checked as kNew (or replayed from the journal).
  void record(const DeviceId& device, SourcePosition pos);

 private:
  struct BootWindow {
    std::uint64_t highest = 0;
    bool any = false;
    std::bitset<kWindow> seen;
  };
  using Boots = std::map<std::uint32_t, BootWindow>;  // ordered by epoch
  std::unordered_map<DeviceId, Boots> devices_;
};

}  // namespace talos
