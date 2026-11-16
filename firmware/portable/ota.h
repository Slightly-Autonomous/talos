// A/B OTA state machine (spec §36, ADR 0013). Mirrors the ESP-IDF model:
// two app slots, pending-verify after switching, confirm or roll back.
// Every transition is persisted before it takes effect so a reset at any
// point resumes or rolls back safely.
//
//   idle --offer--> downloading --all bytes + digest ok--> ready_to_boot
//   ready_to_boot --reboot--> pending_verify (running new slot)
//   pending_verify --health confirmed--> idle (new slot is active)
//   pending_verify --reboot before confirm--> rolled_back (old slot)
//   downloading --digest mismatch--> failed
//
// Signature verification of the release manifest is Phase 6; today the
// image digest from the (to-be-signed) offer is checked.
#pragma once

#include <array>
#include <cstdint>

#include "firmware/portable/hal.h"
#include "firmware/portable/types.h"

namespace talos::fw {

enum class OtaState : std::uint8_t {
  kIdle = 0,
  kDownloading = 1,
  kReadyToBoot = 2,
  kPendingVerify = 3,
  kFailed = 4,
  kRolledBack = 5,
};

struct OtaRecord {
  OtaState state = OtaState::kIdle;
  std::uint8_t active_slot = 0;  // slot the bootloader runs when not trialing
  std::uint8_t target_slot = 1;  // slot being written / trialed
  std::uint8_t reserved = 0;
  std::uint32_t active_version = 1;
  std::uint32_t target_version = 0;
  std::uint32_t size = 0;
  std::uint32_t received = 0;
  std::array<std::uint8_t, 32> sha256{};
  bool operator==(const OtaRecord&) const = default;
};

struct BootDecision {
  std::uint8_t slot = 0;
  std::uint32_t version = 1;
  bool rolled_back = false;
};

class Ota {
 public:
  explicit Ota(Platform& p) : p_(p) {}

  // Bootloader step: loads the record, decides which slot runs, persists.
  BootDecision boot();
  // Returns false (and changes nothing) if not acceptable right now.
  bool offer(const OtaOffer& offer, bool power_ok);
  // Accepts only the next expected offset; resumable after reset.
  bool chunk(const OtaChunk& chunk);
  // The new image proved healthy (e.g. it reached the hub): make it active.
  bool confirm();

  const OtaRecord& record() const { return rec_; }
  bool reboot_wanted() const { return rec_.state == OtaState::kReadyToBoot; }
  std::uint32_t running_version() const { return running_version_; }

 private:
  bool save();
  bool verify_digest();

  Platform& p_;
  OtaRecord rec_;
  std::uint32_t running_version_ = 1;
};

}  // namespace talos::fw
