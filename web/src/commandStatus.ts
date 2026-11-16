import { CommandStatus } from "@talos/contracts/talos/v1/command_pb";

export type Outcome = "pending" | "verified" | "unverified" | "failed" | "unknown";

// Mirrors core/execution outcome_of(). Only VERIFIED is success; the UI must
// never render an unverified command as done (security invariant #6).
export function outcomeOf(status: CommandStatus): Outcome {
  switch (status) {
    case CommandStatus.VERIFIED:
      return "verified";
    case CommandStatus.COMPLETED_UNVERIFIED:
      return "unverified";
    case CommandStatus.DENIED:
    case CommandStatus.EXPIRED:
    case CommandStatus.CANCELED:
    case CommandStatus.REJECTED:
      return "failed";
    case CommandStatus.UNKNOWN_RESULT:
      return "unknown";
    case CommandStatus.REQUESTED:
    case CommandStatus.ACCEPTED:
    case CommandStatus.DISPATCHED:
    case CommandStatus.DEVICE_ACKNOWLEDGED:
      return "pending";
    default:
      return "unknown";
  }
}
