#include "core/events/observation.h"

namespace talos {

Status validate_envelope(const ObservationEnvelope& env) {
  auto bad = [](std::string msg) { return Error{"invalid_envelope", std::move(msg)}; };
  if (env.schema_version != 1) {
    return Error{"unsupported_schema", "schema_version " + std::to_string(env.schema_version)};
  }
  if (env.property.empty()) return bad("property is empty");
  if (env.source.device.empty()) return bad("source device is empty");
  if (env.source.capability.empty()) return bad("source capability is empty");
  if (env.event_type.empty()) return bad("event_type is empty");
  if (env.event_id.empty()) return bad("event_id is empty");
  if (std::holds_alternative<std::monostate>(env.payload)) return bad("payload is empty");
  if (env.source.slot && *env.source.slot >= 4) return bad("slot out of range");
  return Status::Ok();
}

}  // namespace talos
