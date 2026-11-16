#include "firmware/portable/ota.h"

#include <cstring>

#include "common/sha256/sha256.h"

namespace talos::fw {

bool Ota::save() {
  return p_.store.put(StoreKey::kOta, reinterpret_cast<const std::uint8_t*>(&rec_), sizeof rec_);
}

BootDecision Ota::boot() {
  OtaRecord stored;
  if (auto n =
          p_.store.get(StoreKey::kOta, reinterpret_cast<std::uint8_t*>(&stored), sizeof stored);
      n && *n == sizeof stored) {
    rec_ = stored;
  }
  BootDecision d{rec_.active_slot, rec_.active_version, false};
  switch (rec_.state) {
    case OtaState::kReadyToBoot:
      // Trial boot of the new image; it must confirm before the next reboot.
      rec_.state = OtaState::kPendingVerify;
      save();
      d = {rec_.target_slot, rec_.target_version, false};
      break;
    case OtaState::kPendingVerify:
      // Rebooted without confirming: the trial failed. Stay on the old slot.
      rec_.state = OtaState::kRolledBack;
      save();
      d.rolled_back = true;
      break;
    case OtaState::kDownloading:
      break;  // resume the download on the old image
    default:
      break;
  }
  running_version_ = d.version;
  return d;
}

bool Ota::offer(const OtaOffer& offer, bool power_ok) {
  if (!power_ok) return false;
  if (rec_.state == OtaState::kDownloading || rec_.state == OtaState::kReadyToBoot ||
      rec_.state == OtaState::kPendingVerify) {
    // Already busy; re-offering the same image resumes.
    return rec_.state == OtaState::kDownloading && offer.version == rec_.target_version &&
           offer.sha256 == rec_.sha256;
  }
  if (offer.size == 0 || offer.size > p_.flash.slot_capacity() ||
      offer.version <= rec_.active_version) {
    return false;  // empty, too large, or a downgrade
  }
  rec_.target_slot = static_cast<std::uint8_t>(rec_.active_slot ^ 1u);
  if (!p_.flash.erase(rec_.target_slot)) return false;
  rec_.state = OtaState::kDownloading;
  rec_.target_version = offer.version;
  rec_.size = offer.size;
  rec_.received = 0;
  rec_.sha256 = offer.sha256;
  return save();
}

bool Ota::verify_digest() {
  Sha256 h;
  std::uint8_t buf[256];
  for (std::uint32_t off = 0; off < rec_.size;) {
    const std::uint32_t n = (rec_.size - off) < sizeof buf ? (rec_.size - off) : sizeof buf;
    if (!p_.flash.read(rec_.target_slot, off, buf, n)) return false;
    h.update(buf, n);
    off += n;
  }
  return h.finish() == rec_.sha256;
}

bool Ota::chunk(const OtaChunk& c) {
  if (rec_.state != OtaState::kDownloading || c.offset != rec_.received || c.length == 0 ||
      c.length > kOtaChunkMax || rec_.received + c.length > rec_.size) {
    return false;
  }
  if (!p_.flash.write(rec_.target_slot, c.offset, c.data.data(), c.length)) return false;
  rec_.received += c.length;
  if (rec_.received == rec_.size) {
    rec_.state = verify_digest() ? OtaState::kReadyToBoot : OtaState::kFailed;
  }
  return save();
}

bool Ota::confirm() {
  if (rec_.state != OtaState::kPendingVerify) return false;
  rec_.state = OtaState::kIdle;
  rec_.active_slot = rec_.target_slot;
  rec_.active_version = rec_.target_version;
  return save();
}

}  // namespace talos::fw
