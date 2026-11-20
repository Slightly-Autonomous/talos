// Command requests the UI sends. They carry the operation, target and args
// only: the hub derives the expected observation (what "verified" means) and
// checks freshness on the target itself, so the client never sends `expect`.
import { create } from "@bufbuild/protobuf";
import { CommandRequestSchema, type CommandRequest } from "@talos/contracts/talos/api/v1/api_pb";

export function lockCommandRequest(
  deviceId: string,
  locked: boolean,
  nowMs: number,
): CommandRequest {
  return create(CommandRequestSchema, {
    commandId: `ui-lock-${nowMs}`,
    operation: "access.set_locked",
    target: { deviceId, capabilityId: "access.lock" },
    args: { kind: { case: "setLocked", value: { locked } } },
    ttlMs: 5000,
    verificationWindowMs: 5000,
  });
}
