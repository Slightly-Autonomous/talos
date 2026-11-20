// Operation catalog: the single source of truth for what each command
// operation is (ADR 0004).
//
// Devices act on a command's args, so the operation string a caller sends is
// only trustworthy if the hub checks that it agrees with everything else:
//  * the one CommandArgs alternative the operation takes,
//  * the capability the command must target,
//  * the risk class policy uses,
//  * the longest TTL allowed for that risk,
//  * and the observation that proves the effect, derived here and never
//    taken from a caller.
// A command that disagrees with its catalog entry is refused before policy
// runs, so a low-risk operation name can never carry high-risk args.
#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

#include "core/base/result.h"
#include "core/execution/command.h"
#include "core/time/time.h"

namespace talos {

struct OperationSpec {
  std::string_view operation;
  RiskClass risk = RiskClass::kSecurityCritical;
  // False for operations with a risk class but no device contract yet
  // (no typed args, no adapter): requests for them are refused.
  bool dispatchable = false;
  // Index of the CommandArgs alternative this operation takes.
  std::size_t args_index = 0;
  std::string_view target_capability;
};

// nullptr for operations that are not in the catalog.
const OperationSpec* find_operation(std::string_view operation);

// Longest dispatch TTL a command of this risk may ask for.
Millis max_ttl_for(RiskClass risk);
inline constexpr Millis kMaxVerificationWindow{5 * 60 * 1000};
inline constexpr std::size_t kMaxSpeakTextLen = 280;
inline constexpr std::size_t kMaxNotifyTextLen = 1024;

// The observation that verifies `command`, derived from its operation, args
// and target. nullopt when the operation has no observable effect (notify):
// such commands can at best complete unverified.
std::optional<ExpectedObservation> derive_expectation(const Command& command);

// Checks `command` against the catalog and returns it normalized: `expect`
// is the derived observation, and `verification` is ack-only when the caller
// asked for ack-only or the operation has nothing to observe. Error codes
// (stable, used as the denial reason): unsupported_operation,
// args_do_not_match_operation, target_does_not_match_operation,
// expectation_does_not_match_operation, ttl_out_of_bounds,
// verification_window_out_of_bounds.
Result<Command> normalize_command(const Command& command);

}  // namespace talos
