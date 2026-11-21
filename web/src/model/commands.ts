// Command requests the UI sends. They carry the operation, target and args
// only: the hub derives the expected observation (what "verified" means) and
// checks freshness on the target itself, so the client never sends `expect`.
import { create } from "@bufbuild/protobuf";
import { HubError } from "@talos/contracts/client";
import {
  CommandRequestSchema,
  type CommandRequest,
  type CommandResponse,
} from "@talos/contracts/talos/api/v1/api_pb";

// Unguessable: a timestamp id could be pre-claimed by another principal,
// turning the owner's command into a 409 (red team #6).
export function newCommandId(prefix: string): string {
  return `${prefix}-${crypto.randomUUID()}`;
}

export function lockCommandRequest(
  deviceId: string,
  locked: boolean,
  commandId: string = newCommandId("ui-lock"),
): CommandRequest {
  return create(CommandRequestSchema, {
    commandId,
    operation: "access.set_locked",
    target: { deviceId, capabilityId: "access.lock" },
    args: { kind: { case: "setLocked", value: { locked } } },
    ttlMs: 5000,
    verificationWindowMs: 5000,
  });
}

interface CommandSender {
  requestCommand(req: CommandRequest): Promise<CommandResponse>;
}

// A 409 means the id belongs to another principal's command (the hub reveals
// nothing about it). It is never our command, so never report it as sent:
// retry once under a fresh id, and surface a second conflict as an error.
export async function requestLock(
  client: CommandSender,
  deviceId: string,
  locked: boolean,
  makeId: () => string = () => newCommandId("ui-lock"),
): Promise<CommandResponse> {
  try {
    return await client.requestCommand(lockCommandRequest(deviceId, locked, makeId()));
  } catch (e) {
    if (!(e instanceof HubError) || e.status !== 409) throw e;
    return client.requestCommand(lockCommandRequest(deviceId, locked, makeId()));
  }
}

export const DEFAULT_MAINTENANCE_MS = 30 * 60 * 1000;

// device.maintenance (hub-internal): suppresses this node's tamper alarm for a
// bounded time. Humans only; the hub refuses agents and out-of-range durations.
export function maintenanceCommandRequest(
  deviceId: string,
  durationMs: number = DEFAULT_MAINTENANCE_MS,
  commandId: string = newCommandId("ui-maint"),
): CommandRequest {
  return create(CommandRequestSchema, {
    commandId,
    operation: "device.maintenance",
    target: { deviceId, capabilityId: "device.health" },
    args: { kind: { case: "setMaintenance", value: { durationMs } } },
    ttlMs: 10_000,
  });
}

export function endMaintenanceCommandRequest(
  deviceId: string,
  commandId: string = newCommandId("ui-maint-end"),
): CommandRequest {
  return create(CommandRequestSchema, {
    commandId,
    operation: "device.maintenance_end",
    target: { deviceId, capabilityId: "device.health" },
    ttlMs: 10_000,
  });
}
