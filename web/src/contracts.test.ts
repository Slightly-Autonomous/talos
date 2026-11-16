import { create, fromBinary, toBinary } from "@bufbuild/protobuf";
import { CommandStatus } from "./gen/talos/v1/command_pb";
import { ObservationEnvelopeSchema } from "./gen/talos/v1/observation_pb";
import { outcomeOf } from "./commandStatus";

test("generated contract types round-trip through binary", () => {
  const env = create(ObservationEnvelopeSchema, {
    schemaVersion: 1,
    propertyId: "house-01",
    eventId: "node-07:1:1",
    eventType: "motion.detected",
    source: { deviceId: "node-07", slot: 0, capabilityId: "motion.detect" },
    position: { bootEpoch: 1, sequence: 1n },
    payload: { kind: { case: "motion", value: { detected: true } } },
  });
  const back = fromBinary(ObservationEnvelopeSchema, toBinary(ObservationEnvelopeSchema, env));
  expect(back.eventId).toBe("node-07:1:1");
  expect(back.payload?.kind.case).toBe("motion");
});

test("only VERIFIED counts as success", () => {
  expect(outcomeOf(CommandStatus.VERIFIED)).toBe("verified");
  expect(outcomeOf(CommandStatus.COMPLETED_UNVERIFIED)).toBe("unverified");
  expect(outcomeOf(CommandStatus.DEVICE_ACKNOWLEDGED)).toBe("pending");
  expect(outcomeOf(CommandStatus.UNKNOWN_RESULT)).toBe("unknown");
});
