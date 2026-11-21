#include "core/execution/operations.h"

#include <string>
#include <type_traits>
#include <variant>

namespace talos {

namespace {

constexpr std::size_t kNoArgs = 0;  // std::monostate

// Phase 1 catalog. Anything absent fails closed (security-critical, refused).
constexpr OperationSpec kCatalog[] = {
    {"state.read", RiskClass::kInformational, false, kNoArgs, ""},
    {"camera.snapshot", RiskClass::kSensitiveRead, false, kNoArgs, ""},
    {"notify", RiskClass::kBenignOutput, true, 3, "notify"},
    {"light.set_on", RiskClass::kBenignOutput, false, kNoArgs, ""},
    {"audio.speak", RiskClass::kDisruptiveOutput, true, 1, "audio.speak"},
    {"alarm.activate", RiskClass::kDisruptiveOutput, false, kNoArgs, ""},
    {"access.set_locked", RiskClass::kSecurityCritical, true, 2, "access.lock"},
    {"alarm.disarm", RiskClass::kSecurityCritical, false, kNoArgs, ""},
    // Suppresses one node's tamper alarm for a bounded time: it can silence an
    // alarm, so humans only (security-critical) and the node must be reporting
    // (fresh device.health). Ending it raises protection again: benign.
    {"device.maintenance", RiskClass::kSecurityCritical, true, 4, "device.health", true},
    {"device.maintenance_end", RiskClass::kBenignOutput, true, kNoArgs, "device.health", true},
};

static_assert(std::variant_size_v<CommandArgs> == 5, "update the operation catalog");
static_assert(std::is_same_v<std::variant_alternative_t<4, CommandArgs>, SetMaintenance>);
static_assert(std::is_same_v<std::variant_alternative_t<1, CommandArgs>, AudioSpeak>);
static_assert(std::is_same_v<std::variant_alternative_t<2, CommandArgs>, SetLocked>);
static_assert(std::is_same_v<std::variant_alternative_t<3, CommandArgs>, Notify>);

Error reject(std::string code, std::string message) {
  return Error{std::move(code), std::move(message)};
}

}  // namespace

const OperationSpec* find_operation(std::string_view operation) {
  for (const auto& e : kCatalog) {
    if (e.operation == operation) return &e;
  }
  return nullptr;
}

Millis max_ttl_for(RiskClass risk) {
  // A security-critical command must not sit queued long enough to surprise
  // anyone; everything else gets a few minutes for a slow adapter.
  return risk == RiskClass::kSecurityCritical ? Millis{30'000} : Millis{5 * 60 * 1000};
}

std::optional<ExpectedObservation> derive_expectation(const Command& command) {
  if (std::holds_alternative<AudioSpeak>(command.args)) {
    return ExpectedObservation{command.target, AudioPlayback{true}};
  }
  if (const auto* lock = std::get_if<SetLocked>(&command.args)) {
    return ExpectedObservation{command.target, LockState{lock->locked}};
  }
  return std::nullopt;  // notify and no-args operations have nothing to observe
}

Result<Command> normalize_command(const Command& command) {
  const OperationSpec* spec = find_operation(command.operation);
  if (!spec || !spec->dispatchable) {
    return reject("unsupported_operation", "operation '" + command.operation + "'");
  }
  if (command.args.index() != spec->args_index) {
    return reject("args_do_not_match_operation", "args do not fit " + command.operation);
  }
  // Bounded free text: it is journaled, spoken and shown to people and agents.
  if (const auto* speak = std::get_if<AudioSpeak>(&command.args);
      speak && (speak->text.size() > kMaxSpeakTextLen || speak->volume_percent > 100)) {
    return reject("args_out_of_bounds", "audio.speak text is at most 280 bytes, volume 0-100");
  }
  if (const auto* n = std::get_if<Notify>(&command.args);
      n && (n->text.size() > kMaxNotifyTextLen || n->recipient_role.size() > 64)) {
    return reject("args_out_of_bounds", "notify text is at most 1024 bytes, role 64");
  }
  if (command.target.capability.str() != spec->target_capability) {
    return reject("target_does_not_match_operation",
                  command.operation + " must target " + std::string(spec->target_capability));
  }
  Command out = command;
  if (auto* m = std::get_if<SetMaintenance>(&out.args)) {
    if (m->duration_ms == 0) m->duration_ms = static_cast<std::uint32_t>(kDefaultMaintenance.count);
    if (Millis{m->duration_ms} < kMinMaintenance || Millis{m->duration_ms} > kMaxMaintenance) {
      return reject("args_out_of_bounds", "maintenance lasts 1 min to 4 h");
    }
  }
  const Millis ttl = command.expires_at - command.created_at;
  if (ttl > max_ttl_for(spec->risk)) {
    return reject("ttl_out_of_bounds", "ttl " + std::to_string(ttl.count) + " ms");
  }
  if (command.verification_window.count < 0 ||
      command.verification_window > kMaxVerificationWindow) {
    return reject("verification_window_out_of_bounds",
                  "window " + std::to_string(command.verification_window.count) + " ms");
  }
  const auto derived = derive_expectation(command);
  if (command.expect && command.expect != derived) {
    return reject("expectation_does_not_match_operation",
                  "expect must be what " + command.operation + " produces");
  }
  if (spec->hub_internal) {
    // The hub's own journaled state change is the evidence (ADR 0007 amendment).
    out.verification = VerificationMode::kObservedState;
    out.expect.reset();
    return out;
  }
  if (command.verification == VerificationMode::kAckOnly || !derived) {
    out.verification = VerificationMode::kAckOnly;
    out.expect.reset();  // ack-only can never be verified
  } else {
    out.verification = VerificationMode::kObservedState;
    out.expect = derived;
  }
  return out;
}

}  // namespace talos
