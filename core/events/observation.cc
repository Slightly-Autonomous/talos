#include "core/events/observation.h"

namespace talos {

namespace {

bool all_of(const std::string& s, bool (*ok)(char)) {
  for (char c : s) {
    if (!ok(c)) return false;
  }
  return true;
}

bool id_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == ':' || c == '@' || c == '.' || c == '-';
}

bool event_type_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_';
}

bool label_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-' || c == ' ' || c == '.';
}

}  // namespace

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
  // Device-controlled text ends up in rule command ids, the UI and agent
  // tool output: bound it and keep it to a plain character set.
  if (env.event_type.size() > kMaxEventTypeLen || !all_of(env.event_type, event_type_char)) {
    return bad("event_type must be 1-64 of [a-z0-9._]");
  }
  if (env.event_id.str().size() > kMaxEventIdLen || !all_of(env.event_id.str(), id_char)) {
    return bad("event_id must be 1-128 of [A-Za-z0-9_:@.-]");
  }
  if (env.causation &&
      (env.causation->str().size() > kMaxEventIdLen || !all_of(env.causation->str(), id_char))) {
    return bad("causation must be 1-128 of [A-Za-z0-9_:@.-]");
  }
  if (const auto* o = std::get_if<ObjectDetected>(&env.payload);
      o && (o->label.size() > kMaxLabelLen || !all_of(o->label, label_char))) {
    return bad("object label must be at most 64 of [A-Za-z0-9 _.-]");
  }
  if (std::holds_alternative<std::monostate>(env.payload)) return bad("payload is empty");
  if (env.source.slot && *env.source.slot >= 4) return bad("slot out of range");
  return Status::Ok();
}

}  // namespace talos
