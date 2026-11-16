#include "core/execution/command.h"

namespace talos {

std::string_view to_string(CommandStatus s) {
  switch (s) {
    case CommandStatus::kRequested:
      return "requested";
    case CommandStatus::kDenied:
      return "denied";
    case CommandStatus::kAccepted:
      return "accepted";
    case CommandStatus::kExpired:
      return "expired";
    case CommandStatus::kCanceled:
      return "canceled";
    case CommandStatus::kDispatched:
      return "dispatched";
    case CommandStatus::kDeviceAcknowledged:
      return "device_acknowledged";
    case CommandStatus::kRejected:
      return "rejected";
    case CommandStatus::kCompletedUnverified:
      return "completed_unverified";
    case CommandStatus::kVerified:
      return "verified";
    case CommandStatus::kUnknownResult:
      return "unknown_result";
  }
  return "invalid";
}

CommandOutcome outcome_of(CommandStatus s) {
  switch (s) {
    case CommandStatus::kVerified:
      return CommandOutcome::kVerified;
    case CommandStatus::kCompletedUnverified:
      return CommandOutcome::kUnverified;
    case CommandStatus::kDenied:
    case CommandStatus::kExpired:
    case CommandStatus::kCanceled:
    case CommandStatus::kRejected:
      return CommandOutcome::kFailed;
    case CommandStatus::kUnknownResult:
      return CommandOutcome::kUnknown;
    case CommandStatus::kRequested:
    case CommandStatus::kAccepted:
    case CommandStatus::kDispatched:
    case CommandStatus::kDeviceAcknowledged:
      return CommandOutcome::kPending;
  }
  return CommandOutcome::kUnknown;
}

bool is_terminal_before_dispatch(CommandStatus s) {
  return s == CommandStatus::kDenied || s == CommandStatus::kExpired ||
         s == CommandStatus::kCanceled;
}

}  // namespace talos
