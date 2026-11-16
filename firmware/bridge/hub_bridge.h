// Bridges portable firmware events to hub observation envelopes (L2 twin).
// The Phase 6 CoAP/Thread adapter performs the same mapping on the wire.
#pragma once

#include <optional>

#include "core/events/observation.h"
#include "firmware/portable/types.h"

namespace talos::fw {

// Domain events become observations; diagnostics (module presence/faults,
// config, OTA, power mode) return nullopt here and belong to the
// observability plane (Phase 6). They are still ACKed so they never retry
// forever.
std::optional<ObservationEnvelope> to_envelope(const DeviceEvent& e, const DeviceId& device,
                                               const PropertyId& property);

}  // namespace talos::fw
