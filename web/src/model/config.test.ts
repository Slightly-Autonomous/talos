import { create } from "@bufbuild/protobuf";
import { describe, expect, test } from "vitest";
import { PropertySchema } from "@talos/contracts/talos/v1/property_pb";
import { ArmMode } from "@talos/contracts/talos/v1/rule_pb";
import {
  draftFromRule,
  emptyDraft,
  layoutDiff,
  parseHhMm,
  ruleFromDraft,
  snap,
  zoneAt,
} from "./config";

const property = create(PropertySchema, {
  id: "house-01",
  zones: [
    {
      id: "hallway",
      name: "Hallway",
      polygon: [
        { x: 0, y: 0 },
        { x: 4, y: 0 },
        { x: 4, y: 10 },
        { x: 0, y: 10 },
      ],
    },
  ],
  devices: [{ id: "node-12", zoneId: "hallway", position: { x: 1, y: 1 } }],
});

test("time parsing and snapping", () => {
  expect(parseHhMm("22:30")).toBe(1350);
  expect(parseHhMm("24:00")).toBeUndefined();
  expect(parseHhMm("7")).toBeUndefined();
  expect(snap(1.26)).toBe(1.5);
});

test("zoneAt uses polygons", () => {
  expect(zoneAt(property, { x: 2, y: 5 })).toBe("hallway");
  expect(zoneAt(property, { x: 9, y: 5 })).toBeUndefined();
});

describe("rule drafts", () => {
  const draft = {
    ...emptyDraft(),
    id: "front_door_chime",
    triggerEventType: "contact.opened",
    zoneId: "hallway",
    modes: [ArmMode.ARMED_AWAY],
    schedule: { start: "22:00", end: "06:00" },
    targetDeviceId: "patio_speaker",
    text: "Front door opened",
  };

  test("builds a verified speak rule with its own principal", () => {
    const rule = ruleFromDraft(draft, "house-01");
    if (typeof rule === "string") throw new Error(rule);
    expect(rule.principalId).toBe("rule:front_door_chime");
    expect(rule.schedule).toMatchObject({ startMinute: 1320, endMinute: 360 });
    expect(rule.actions[0]?.expect?.payload?.kind.case).toBe("audioPlayback");
    expect(draftFromRule(rule)).toEqual(draft);
  });

  test("a camera bookmark targets camera.recording, is verified by its clip, needs no text", () => {
    const b = ruleFromDraft(
      { ...draft, action: "bookmark", targetDeviceId: "cam_patio_video", text: "" },
      "house-01",
    );
    if (typeof b === "string") throw new Error(b);
    const a = b.actions[0]!;
    expect(a.operation).toBe("camera.bookmark");
    expect(a.target?.capabilityId).toBe("camera.recording");
    expect(a.args?.kind).toEqual({
      case: "bookmark",
      value: expect.objectContaining({ durationS: 30, label: "talos" }),
    });
    expect(a.verificationWindowMs).toBe(180000n);
    expect(a.expect).toBeUndefined(); // the hub derives the clip expectation (ADR 0020)
    expect(draftFromRule(b).action).toBe("bookmark");
  });

  test("notify is ack-only and validation errors are explained", () => {
    const n = ruleFromDraft({ ...draft, action: "notify" }, "house-01");
    if (typeof n === "string") throw new Error(n);
    expect(n.actions[0]?.expect).toBeUndefined();
    expect(ruleFromDraft({ ...draft, id: "Bad Id" }, "house-01")).toMatch(/Rule id/);
    expect(ruleFromDraft({ ...draft, targetDeviceId: "" }, "house-01")).toMatch(/target/);
    expect(ruleFromDraft({ ...draft, schedule: { start: "x", end: "06:00" } }, "house-01")).toMatch(
      /HH:MM/,
    );
  });
});

test("layout diff summarizes changes", () => {
  const after = create(PropertySchema, {
    id: "house-01",
    zones: [
      { ...property.zones[0]!, name: "Entry" },
      { id: "attic", name: "Attic" },
    ],
    devices: [{ id: "node-12", zoneId: "hallway", position: { x: 2, y: 1 } }],
  });
  expect(layoutDiff(property, after)).toEqual([
    'rename hallway: "Hallway" → "Entry"',
    "add zone attic",
    "move node-12",
  ]);
});
