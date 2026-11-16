// Shared test fixtures for core tests (testonly).
#pragma once

#include <cstdint>
#include <string>

#include "core/domain/property.h"
#include "core/events/observation.h"

namespace talos::testing {

// house-01: zones backyard/patio/hallway; node-07 (backyard), node-12
// (hallway, front door contact), patio_speaker, lock-3.
Property sample_property();

ObservationEnvelope motion(const std::string& device, std::uint32_t boot, std::uint64_t seq,
                           bool detected = true);
ObservationEnvelope contact(const std::string& device, std::uint32_t boot, std::uint64_t seq,
                            bool open);

}  // namespace talos::testing
