import { create, toJson } from "@bufbuild/protobuf";
import { HubError } from "@talos/contracts/client";
import {
  CommandRequestSchema,
  CommandResponseSchema,
  type CommandRequest,
} from "@talos/contracts/talos/api/v1/api_pb";
import { lockCommandRequest, newCommandId, requestLock } from "./commands";

test("lock requests never carry a client-chosen expect", () => {
  const req = lockCommandRequest("lock-3", false, "ui-lock-42");
  expect(req.expect).toBeUndefined();
  expect(toJson(CommandRequestSchema, req)).not.toHaveProperty("expect");
  expect(req).toMatchObject({
    commandId: "ui-lock-42",
    operation: "access.set_locked",
    target: { deviceId: "lock-3", capabilityId: "access.lock" },
    args: { kind: { case: "setLocked", value: { locked: false } } },
  });
});

// Red team #6: ids were ui-lock-<ms>, so another principal could pre-claim them.
test("command ids are unguessable", () => {
  const a = lockCommandRequest("lock-3", true).commandId;
  const b = lockCommandRequest("lock-3", true).commandId;
  expect(a).not.toBe(b);
  expect(a).toMatch(/^ui-lock-[0-9a-f-]{36}$/);
  expect(newCommandId("x")).not.toMatch(/^x-\d+$/);
});

test("a 409 for another principal's id is retried under a fresh id, never reported as sent", async () => {
  const sent: CommandRequest[] = [];
  let conflicts = 1;
  const client = {
    requestCommand: (req: CommandRequest) => {
      sent.push(req);
      if (conflicts-- > 0) return Promise.reject(new HubError(409, "conflict", "in use"));
      return Promise.resolve(create(CommandResponseSchema, { commandId: req.commandId }));
    },
  };
  const ids = ["id-1", "id-2"];
  const r = await requestLock(client, "lock-3", true, () => ids.shift()!);
  expect(sent.map((s) => s.commandId)).toEqual(["id-1", "id-2"]);
  expect(r.commandId).toBe("id-2");

  conflicts = 2;
  await expect(requestLock(client, "lock-3", true)).rejects.toBeInstanceOf(HubError);
});
