// Commands and their lifecycle (spec §23, ADR 0004).
//
// Key semantics:
//  * expires_at (TTL) gates DISPATCH only. A command that is not dispatched by
//    then becomes `expired` and is never sent.
//  * Verification is separate: after dispatch, ACKs and observations are
//    collected until dispatched_at + verification_window. When that window
//    closes without conclusive evidence the status becomes `unknown_result`
//    (no ACK, no confirming observation) or `completed_unverified` (ACK only).
//  * Evidence that arrives after the window is still recorded faithfully, with
//    late=true, and can move the command to its true outcome.
//  * Only `verified` counts as success.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/domain/capability.h"
#include "core/domain/ids.h"
#include "core/events/observation.h"
#include "core/time/time.h"

namespace talos {

enum class RiskClass : std::uint8_t {
  kInformational = 0,
  kSensitiveRead = 1,
  kBenignOutput = 2,
  kDisruptiveOutput = 3,
  kSecurityCritical = 4,
};

enum class CommandStatus : std::uint8_t {
  kRequested,
  kDenied,               // terminal: policy refused
  kAccepted,             // authorized, awaiting dispatch
  kExpired,              // terminal: TTL elapsed before dispatch; never sent
  kCanceled,             // terminal: canceled before dispatch
  kDispatched,           // handed to the device adapter
  kDeviceAcknowledged,   // device says it accepted/executed
  kRejected,             // device refused
  kCompletedUnverified,  // ACK received, no confirming observation (or ack-only mode)
  kVerified,             // independent observation matched the expected state
  kUnknownResult,        // dispatched; window closed with no ACK and no observation
};

std::string_view to_string(CommandStatus s);

// Typed command arguments. Capabilities are typed behaviors, never set_value(1).
struct AudioSpeak {
  std::string text;
  std::uint8_t volume_percent = 50;
  auto operator<=>(const AudioSpeak&) const = default;
};
struct SetLocked {
  bool locked = true;
  auto operator<=>(const SetLocked&) const = default;
};
struct Notify {
  std::string recipient_role;
  std::string text;
  auto operator<=>(const Notify&) const = default;
};
// Hub-side maintenance window for one node (device.maintenance): its tamper
// alarm is suppressed for duration_ms (0 = the default, 30 min; at most 4 h).
struct SetMaintenance {
  std::uint32_t duration_ms = 0;
  auto operator<=>(const SetMaintenance&) const = default;
};
// camera.bookmark: mark the recorder's footage around now (ADR 0020).
// duration_s 0 = the default, 30 s (5-120 s); label 1-32 of [a-z0-9_], "" = "talos".
struct Bookmark {
  std::uint32_t duration_s = 0;
  std::string label;
  auto operator<=>(const Bookmark&) const = default;
};
using CommandArgs =
    std::variant<std::monostate, AudioSpeak, SetLocked, Notify, SetMaintenance, Bookmark>;

enum class VerificationMode : std::uint8_t {
  kAckOnly,        // best outcome is completed_unverified (e.g. notify)
  kObservedState,  // needs an observation matching `expect`
};

struct ExpectedObservation {
  CapabilityRef source;
  ObservationPayload payload;
  bool operator==(const ExpectedObservation&) const = default;
};

struct Command {
  CommandId id;
  std::string idempotency_key;
  PrincipalId principal;
  PropertyId property;
  CapabilityRef target;
  std::string operation;  // e.g. "audio.speak", "access.set_locked"
  CommandArgs args;
  Timestamp created_at;
  Timestamp expires_at;  // dispatch deadline
  VerificationMode verification = VerificationMode::kObservedState;
  Millis verification_window{5'000};
  std::optional<ExpectedObservation> expect;
  std::optional<EventId> causation;
  bool operator==(const Command&) const = default;
};

// Policy outcome handed to the ledger. Produced by core/policy.
struct Authorization {
  bool allowed = false;
  std::string reason;
};

struct CommandAck {
  CommandId command;
  bool accepted = true;
  std::string detail;
};

// One journaled lifecycle step. Replaying transitions rebuilds the ledger.
struct CommandTransition {
  CommandId command;
  CommandStatus to = CommandStatus::kRequested;
  Timestamp at;
  std::string reason;
  bool late = false;                // evidence arrived after the verification window
  std::optional<Command> created;   // set only on kRequested
  std::optional<EventId> evidence;  // observation that verified the command
  bool operator==(const CommandTransition&) const = default;
};

// What a status means to a user or caller.
enum class CommandOutcome : std::uint8_t {
  kPending,
  kVerified,    // the only success
  kUnverified,  // ACKed but not independently confirmed
  kFailed,      // denied, rejected, expired, canceled
  kUnknown,     // dispatched, no evidence either way
};

CommandOutcome outcome_of(CommandStatus s);
bool is_terminal_before_dispatch(CommandStatus s);

}  // namespace talos
