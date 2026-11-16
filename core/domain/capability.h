#pragma once

#include <compare>
#include <cstdint>
#include <optional>

#include "core/domain/ids.h"

namespace talos {

// Physical slot index on a controller (0..3 on the four-slot controller).
using SlotIndex = std::uint8_t;

// Addresses one capability instance: a capability type exposed by a device,
// optionally through a module in a specific slot.
struct CapabilityRef {
  DeviceId device;
  std::optional<SlotIndex> slot;
  CapabilityId capability;

  auto operator<=>(const CapabilityRef&) const = default;
};

}  // namespace talos
