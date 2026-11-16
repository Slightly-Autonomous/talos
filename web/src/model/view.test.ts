import { create } from "@bufbuild/protobuf";
import { describe, expect, test } from "vitest";
import {
  CommandOutcome,
  Freshness,
  StateResponseSchema,
} from "@talos/contracts/talos/api/v1/api_pb";
import { JournalEntrySchema } from "@talos/contracts/talos/v1/journal_pb";
import { PropertySchema } from "@talos/contracts/talos/v1/property_pb";
import {
  bounds,
  describeEntry,
  deviceHealth,
  motionHeat,
  outcomeBadge,
  portalStatus,
  zoneActivity,
  zoneOf,
} from "./view";

const property = create(PropertySchema, {
  id: "house-01",
  zones: [
    {
      id: "backyard",
      polygon: [
        { x: 0, y: 13 },
        { x: 12, y: 25 },
      ],
    },
    {
      id: "hallway",
      polygon: [
        { x: 0, y: 0 },
        { x: 4, y: 10 },
      ],
    },
  ],
  devices: [
    { id: "node-07", zoneId: "backyard", position: { x: 6, y: 21 } },
    { id: "node-12", zoneId: "backyard" },
    { id: "lock-3", zoneId: "hallway" },
  ],
  assets: [
    {
      id: "front_door",
      zoneId: "hallway",
      attached: [
        { deviceId: "node-12", slot: 0, capabilityId: "contact.open_state" },
        { deviceId: "lock-3", capabilityId: "access.lock" },
      ],
    },
  ],
});

const NOW = 1_800_000_100_000;

function state(caps: Parameters<typeof create<typeof StateResponseSchema>>[1]) {
  return create(StateResponseSchema, caps);
}

describe("zoneOf", () => {
  test("asset zone wins over device placement", () => {
    expect(
      zoneOf(property, { deviceId: "node-12", slot: 0, capabilityId: "contact.open_state" }),
    ).toBe("hallway");
    expect(zoneOf(property, { deviceId: "node-07", slot: 0, capabilityId: "motion.detect" })).toBe(
      "backyard",
    );
    expect(zoneOf(property, { deviceId: "ghost", capabilityId: "x" })).toBeUndefined();
  });
});

describe("zoneActivity", () => {
  const motion = (detected: boolean, ageMs: number, freshness = Freshness.FRESH) =>
    state({
      capabilities: [
        {
          ref: { deviceId: "node-07", slot: 0, capabilityId: "motion.detect" },
          value: { kind: { case: "motion", value: { detected } } },
          receivedAtUnixMs: BigInt(NOW - ageMs),
          freshness,
        },
      ],
    });

  test("no sensor data is unknown, never clear", () => {
    expect(zoneActivity(property, undefined, NOW).get("backyard")).toBe("unknown");
    expect(zoneActivity(property, state({}), NOW).get("hallway")).toBe("unknown");
  });
  test("detected is active; recently cleared is recent; old is quiet", () => {
    expect(zoneActivity(property, motion(true, 1000), NOW).get("backyard")).toBe("active");
    expect(zoneActivity(property, motion(false, 5000), NOW).get("backyard")).toBe("recent");
    expect(zoneActivity(property, motion(false, 120_000), NOW).get("backyard")).toBe("quiet");
  });
  test("stale motion data is unknown even if it said detected", () => {
    expect(zoneActivity(property, motion(true, 1000, Freshness.STALE), NOW).get("backyard")).toBe(
      "unknown",
    );
  });
});

describe("portalStatus", () => {
  test("combines contact and lock, flags staleness", () => {
    const s = state({
      capabilities: [
        {
          ref: { deviceId: "node-12", slot: 0, capabilityId: "contact.open_state" },
          value: { kind: { case: "contact", value: { open: false } } },
          freshness: Freshness.FRESH,
        },
        {
          ref: { deviceId: "lock-3", capabilityId: "access.lock" },
          value: { kind: { case: "lock", value: { locked: true } } },
          freshness: Freshness.STALE,
        },
      ],
    });
    expect(portalStatus(property, s).get("front_door")).toEqual({
      open: false,
      locked: true,
      stale: true,
    });
    expect(portalStatus(property, undefined).get("front_door")).toEqual({
      open: undefined,
      locked: undefined,
      stale: false,
    });
  });
});

test("device health is unknown without reports", () => {
  expect(deviceHealth(undefined, "node-07")).toBe("unknown");
  expect(
    deviceHealth(
      state({
        capabilities: [
          {
            ref: { deviceId: "node-07", capabilityId: "device.health" },
            freshness: Freshness.STALE,
          },
        ],
      }),
      "node-07",
    ),
  ).toBe("stale");
});

test("only VERIFIED renders as success", () => {
  expect(outcomeBadge(CommandOutcome.VERIFIED)).toEqual({ label: "Verified", tone: "good" });
  expect(outcomeBadge(CommandOutcome.UNVERIFIED).tone).toBe("warn");
  expect(outcomeBadge(CommandOutcome.UNKNOWN).label).toBe("Unknown result");
  expect(outcomeBadge(CommandOutcome.PENDING).tone).toBe("neutral");
});

test("motion heat counts detections per zone within the window", () => {
  const obs = (seq: number, t: number, type: string) =>
    create(JournalEntrySchema, {
      journalSeq: BigInt(seq),
      receivedAtUnixMs: BigInt(t),
      event: {
        case: "observation",
        value: {
          envelope: {
            eventType: type,
            source: { deviceId: "node-07", slot: 0, capabilityId: "motion.detect" },
          },
        },
      },
    });
  const entries = [
    obs(1, 100, "motion.detected"),
    obs(2, 200, "motion.cleared"),
    obs(3, 300, "motion.detected"),
    obs(4, 900, "motion.detected"),
  ];
  expect(motionHeat(property, entries, 0, 500).get("backyard")).toBe(2);
  expect(describeEntry(entries[0]!)).toBe("motion.detected from node-07");
});

test("bounds cover zones and devices", () => {
  expect(bounds(property)).toEqual({ minX: 0, minY: 0, maxX: 12, maxY: 25 });
});
