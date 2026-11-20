#include "core/ingest/source_tracker.h"

namespace talos {

SourceTracker::Check SourceTracker::check(const DeviceId& device, SourcePosition pos,
                                          std::uint32_t epoch_ceiling) const {
  const Check out_of_range{Verdict::kOutOfRange, SourceOrder::kGap};
  if (pos.sequence > kMaxSequence) return out_of_range;
  auto dit = devices_.find(device);
  if (dit == devices_.end() || dit->second.empty()) {
    if (pos.boot_epoch > kMaxFirstEpoch && pos.boot_epoch > epoch_ceiling) return out_of_range;
    return {Verdict::kNew, SourceOrder::kInOrder};
  }
  const Boots& boots = dit->second;
  const std::uint32_t newest_epoch = boots.rbegin()->first;

  auto bit = boots.find(pos.boot_epoch);
  if (bit == boots.end()) {
    if (pos.boot_epoch > newest_epoch) {
      if (pos.boot_epoch - newest_epoch > kMaxEpochJump && pos.boot_epoch > epoch_ceiling) {
        return out_of_range;
      }
      return {Verdict::kNew, SourceOrder::kNewBoot};
    }
    // An older epoch we no longer (or never) track: refuse if it predates
    // everything we remember, otherwise it is a genuinely unseen old boot.
    if (boots.size() >= kMaxBootsPerDevice && pos.boot_epoch < boots.begin()->first) {
      return {Verdict::kTooOld, SourceOrder::kLate};
    }
    return {Verdict::kNew, SourceOrder::kLate};
  }

  const BootWindow& w = bit->second;
  if (!w.any) return {Verdict::kNew, SourceOrder::kInOrder};
  if (pos.sequence > w.highest) {
    if (pos.sequence - w.highest > kMaxSequenceJump) return out_of_range;
    const SourceOrder order =
        pos.sequence == w.highest + 1 ? SourceOrder::kInOrder : SourceOrder::kGap;
    return {Verdict::kNew, pos.boot_epoch < newest_epoch ? SourceOrder::kLate : order};
  }
  if (w.highest - pos.sequence >= kWindow) return {Verdict::kTooOld, SourceOrder::kLate};
  if (w.seen.test(pos.sequence % kWindow)) return {Verdict::kDuplicate, SourceOrder::kLate};
  return {Verdict::kNew, SourceOrder::kLate};
}

void SourceTracker::record(const DeviceId& device, SourcePosition pos) {
  Boots& boots = devices_[device];
  BootWindow& w = boots[pos.boot_epoch];
  if (!w.any) {
    w.any = true;
    w.highest = pos.sequence;
    w.seen.reset();
    w.seen.set(pos.sequence % kWindow);
  } else if (pos.sequence > w.highest) {
    const std::uint64_t jump = pos.sequence - w.highest;
    if (jump >= kWindow) {
      w.seen.reset();
    } else {
      for (std::uint64_t s = w.highest + 1; s < pos.sequence; ++s) w.seen.reset(s % kWindow);
    }
    w.highest = pos.sequence;
    w.seen.set(pos.sequence % kWindow);
  } else if (w.highest - pos.sequence < kWindow) {
    w.seen.set(pos.sequence % kWindow);
  }
  while (boots.size() > kMaxBootsPerDevice) boots.erase(boots.begin());
}

}  // namespace talos
