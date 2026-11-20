// Per-device source-position tracking for deduplication (ADR 0008).
//
// Memory is bounded: for each (device, boot_epoch) we keep the highest
// sequence seen plus a sliding window of which recent sequences arrived. A
// sequence older than the window cannot be proven unique and is refused
// (`kTooOld`) rather than risk a duplicate effect. Only the newest
// kMaxBootsPerDevice boot epochs are remembered per device.
//
// Positions are also bounded (ADR 0008 amendment), so one forged report with
// a huge boot epoch or sequence cannot hold the projection forever:
//  * a sequence is at most UINT32_MAX (the wire format's width) and at most
//    kMaxSequenceJump past the highest seen in its boot;
//  * a new boot epoch is at most kMaxEpochJump past the newest one seen, or
//    at most the caller's `epoch_ceiling` (HTTP adapters use Unix-second
//    epochs, so the hub passes its own time plus a small skew there);
//  * a device with no history accepts epochs up to kMaxFirstEpoch (or the
//    ceiling).
// Anything beyond is kOutOfRange and refused. An owner reset (`reset`) is the
// recovery path for a device whose real counters fell behind its record.
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

  static constexpr std::uint32_t kMaxEpochJump = 64;
  static constexpr std::uint32_t kMaxFirstEpoch = 1u << 20;
  static constexpr std::uint64_t kMaxSequence = 0xFFFF'FFFFull;
  static constexpr std::uint64_t kMaxSequenceJump = 1u << 20;

  enum class Verdict : std::uint8_t { kNew, kDuplicate, kTooOld, kOutOfRange };
  struct Check {
    Verdict verdict = Verdict::kNew;
    SourceOrder order = SourceOrder::kInOrder;  // meaningful when kNew
  };

  Check check(const DeviceId& device, SourcePosition pos, std::uint32_t epoch_ceiling = 0) const;
  // Records a position previously checked as kNew (or replayed from the journal).
  void record(const DeviceId& device, SourcePosition pos);
  // Forgets everything about the device (owner reset).
  void reset(const DeviceId& device) { devices_.erase(device); }

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
