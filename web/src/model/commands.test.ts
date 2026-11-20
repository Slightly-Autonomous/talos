import { toJson } from "@bufbuild/protobuf";
import { CommandRequestSchema } from "@talos/contracts/talos/api/v1/api_pb";
import { lockCommandRequest } from "./commands";

test("lock requests never carry a client-chosen expect", () => {
  const req = lockCommandRequest("lock-3", false, 42);
  expect(req.expect).toBeUndefined();
  expect(toJson(CommandRequestSchema, req)).not.toHaveProperty("expect");
  expect(req).toMatchObject({
    commandId: "ui-lock-42",
    operation: "access.set_locked",
    target: { deviceId: "lock-3", capabilityId: "access.lock" },
    args: { kind: { case: "setLocked", value: { locked: false } } },
  });
});
