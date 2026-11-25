import { create } from "@bufbuild/protobuf";
import { describe, expect, test } from "vitest";
import {
  CommandOutcome,
  Freshness,
  StateResponseSchema,
} from "@talos/contracts/talos/api/v1/api_pb";
import { HealthStatus } from "@talos/contracts/talos/v1/health_pb";
import { JournalEntrySchema } from "@talos/contracts/talos/v1/journal_pb";
import { PropertySchema } from "@talos/contracts/talos/v1/property_pb";
import {
  ageState,
  STATE_MAX_AGE_MS,
  portalDisplay,
  portalLabel,
  portalShowsLocked,
  bounds,
  describeEntry,
  deviceHealth,
  diagnosticLabel,
  deviceTone,
  motionHeat,
  outcomeBadge,
  portalStatus,
  formatRemaining,
  maintenanceFor,
  tamperLabel,
  tamperStatus,
  otherTampers,
  causeLabel,
  zoneActivity,
  zoneOf,
  cameraGroups,
  streamView,
  recentDetections,
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
  // Red team #14: one fresh-quiet PIR must not hide a stale or silent one.
  test("a zone with any stale or never-reported PIR is not quiet", () => {
    const pir = (deviceId: string, detected: boolean, ageMs: number, freshness: Freshness) => ({
      ref: { deviceId, slot: 0, capabilityId: "motion.detect" },
      value: { kind: { case: "motion" as const, value: { detected } } },
      receivedAtUnixMs: BigInt(NOW - ageMs),
      freshness,
    });
    const quiet = pir("node-07", false, 120_000, Freshness.FRESH);
    const dead = pir("node-12", false, 120_000, Freshness.STALE);
    expect(
      zoneActivity(property, state({ capabilities: [quiet, dead] }), NOW).get("backyard"),
    ).toBe("unknown");
    // Order does not matter.
    expect(
      zoneActivity(property, state({ capabilities: [dead, quiet] }), NOW).get("backyard"),
    ).toBe("unknown");
    // Positive evidence still shows.
    const active = pir("node-07", true, 1000, Freshness.FRESH);
    expect(
      zoneActivity(property, state({ capabilities: [active, dead] }), NOW).get("backyard"),
    ).toBe("active");
    // A declared PIR that never reported counts as unknown.
    const declared = create(PropertySchema, {
      id: "house-01",
      zones: property.zones,
      devices: [
        { id: "node-07", zoneId: "backyard", capabilities: ["motion.detect"] },
        { id: "node-12", zoneId: "backyard", capabilities: ["motion.detect"] },
      ],
    });
    expect(zoneActivity(declared, state({ capabilities: [quiet] }), NOW).get("backyard")).toBe(
      "unknown",
    );
    expect(
      zoneActivity(
        declared,
        state({ capabilities: [quiet, pir("node-12", false, 90_000, Freshness.FRESH)] }),
        NOW,
      ).get("backyard"),
    ).toBe("quiet");
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
      openStale: false,
      lockedStale: true,
      stale: true,
    });
    expect(portalStatus(property, undefined).get("front_door")).toEqual({
      open: undefined,
      locked: undefined,
      openStale: false,
      lockedStale: false,
      stale: false,
    });
  });

  const door = (open: boolean, contact: Freshness, locked: boolean, lock: Freshness) =>
    portalStatus(
      property,
      state({
        capabilities: [
          {
            ref: { deviceId: "node-12", slot: 0, capabilityId: "contact.open_state" },
            value: { kind: { case: "contact", value: { open } } },
            freshness: contact,
          },
          {
            ref: { deviceId: "lock-3", capabilityId: "access.lock" },
            value: { kind: { case: "lock", value: { locked } } },
            freshness: lock,
          },
        ],
      }),
    ).get("front_door");

  test("a stale closed door renders unknown, never closed", () => {
    const st = door(false, Freshness.STALE, true, Freshness.FRESH);
    expect(portalDisplay(st)).toBe("unknown");
    expect(portalLabel(st)).toBe("unknown (last: closed), locked");
    expect(portalShowsLocked(st)).toBe(true);
    const unknownFreshness = door(false, Freshness.UNKNOWN, true, Freshness.FRESH);
    expect(portalDisplay(unknownFreshness)).toBe("unknown");
  });

  test("a stale lock is not shown as locked", () => {
    const st = door(false, Freshness.FRESH, true, Freshness.STALE);
    expect(portalDisplay(st)).toBe("closed");
    expect(portalShowsLocked(st)).toBe(false);
    expect(portalLabel(st)).toBe("closed, lock unknown (last: locked)");
  });

  test("fresh reports render as reported; no report is unknown", () => {
    const st = door(true, Freshness.FRESH, false, Freshness.FRESH);
    expect(portalDisplay(st)).toBe("open");
    expect(portalLabel(st)).toBe("open, unlocked");
    expect(portalDisplay(undefined)).toBe("unknown");
    expect(portalLabel(portalStatus(property, undefined).get("front_door"))).toBe("unknown");
  });
});

describe("ageState", () => {
  const fresh = state({
    capabilities: [
      {
        ref: { deviceId: "node-07", slot: 0, capabilityId: "motion.detect" },
        value: { kind: { case: "motion", value: { detected: false } } },
        receivedAtUnixMs: BigInt(NOW - 120_000),
        freshness: Freshness.FRESH,
      },
      {
        ref: { deviceId: "node-12", slot: 0, capabilityId: "contact.open_state" },
        value: { kind: { case: "contact", value: { open: false } } },
        freshness: Freshness.FRESH,
      },
    ],
  });

  test("a recent snapshot from a reachable hub is unchanged", () => {
    expect(ageState(fresh, NOW - 1000, NOW, true)).toBe(fresh);
    expect(
      zoneActivity(property, ageState(fresh, NOW - 1000, NOW, true), NOW).get("backyard"),
    ).toBe("quiet");
  });

  test("state older than the max age renders unknown, not quiet or closed", () => {
    const aged = ageState(fresh, NOW - STATE_MAX_AGE_MS - 1, NOW, true);
    expect(aged?.capabilities.every((c) => c.freshness === Freshness.UNKNOWN)).toBe(true);
    expect(zoneActivity(property, aged, NOW).get("backyard")).toBe("unknown");
    const st = portalStatus(property, aged).get("front_door");
    expect(portalDisplay(st)).toBe("unknown");
    expect(portalLabel(st)).toBe("unknown (last: closed)");
    expect(deviceHealth(aged, "node-12")).toBe("stale");
    // The input is not mutated.
    expect(fresh.capabilities[0]?.freshness).toBe(Freshness.FRESH);
  });

  test("an unreachable hub renders unknown even for a recent snapshot", () => {
    const aged = ageState(fresh, NOW - 1000, NOW, false);
    expect(zoneActivity(property, aged, NOW).get("backyard")).toBe("unknown");
    expect(ageState(fresh, undefined, NOW, true)?.capabilities[1]?.freshness).toBe(
      Freshness.UNKNOWN,
    );
    expect(ageState(undefined, NOW, NOW, true)).toBeUndefined();
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

// HD-032 tamper (ADR 0011): an open enclosure is a fault, never healthy; no report is unknown.
describe("enclosure tamper", () => {
  const withTamper = create(PropertySchema, {
    id: "house-01",
    devices: [
      {
        id: "node-12",
        zoneId: "hallway",
        capabilities: ["contact.open_state", "tamper.enclosure"],
      },
      { id: "node-07", zoneId: "backyard", capabilities: ["motion.detect"] },
    ],
  });
  const tamperCap = (open: boolean, freshness: Freshness) => ({
    ref: { deviceId: "node-12", capabilityId: "tamper.enclosure" },
    freshness,
    value: { kind: { case: "tamper" as const, value: { open } } },
  });
  const heartbeat = {
    ref: { deviceId: "node-12", capabilityId: "device.health" },
    freshness: Freshness.FRESH,
  };

  test("never reported is unknown, not closed; devices without a loop have none", () => {
    const t = tamperStatus(withTamper, state({ capabilities: [heartbeat] }), "node-12");
    expect(t).toEqual({ display: "unknown", last: undefined });
    expect(tamperLabel(t!)).toBe("enclosure unknown");
    expect(tamperStatus(withTamper, undefined, "node-12")?.display).toBe("unknown");
    expect(tamperStatus(withTamper, undefined, "node-07")).toBeUndefined();
  });

  test("open is a fault even next to a fresh heartbeat, and stays one when stale", () => {
    for (const f of [Freshness.FRESH, Freshness.STALE, Freshness.UNKNOWN]) {
      const st = state({ capabilities: [heartbeat, tamperCap(true, f)] });
      expect(tamperStatus(withTamper, st, "node-12")?.display).toBe("open");
      expect(deviceHealth(st, "node-12")).toBe("fault");
    }
    expect(tamperLabel({ display: "open", last: true })).toContain("TAMPER");
  });

  test("closed only while fresh; a stale closed report is unknown", () => {
    const fresh = state({ capabilities: [heartbeat, tamperCap(false, Freshness.FRESH)] });
    expect(tamperStatus(withTamper, fresh, "node-12")?.display).toBe("closed");
    expect(deviceHealth(fresh, "node-12")).toBe("fresh");
    const stale = state({ capabilities: [tamperCap(false, Freshness.STALE)] });
    const t = tamperStatus(withTamper, stale, "node-12")!;
    expect(t.display).toBe("unknown");
    expect(tamperLabel(t)).toBe("enclosure unknown (last: closed)");
  });
});

describe("maintenance", () => {
  const st = state({
    maintenance: [{ deviceId: "node-12", untilUnixMs: BigInt(NOW + 90_000), principalId: "owner" }],
  });
  test("remaining time from the hub's window; over once past it", () => {
    expect(maintenanceFor(st, "node-12", NOW)).toEqual({
      untilMs: NOW + 90_000,
      remainingMs: 90_000,
      principalId: "owner",
    });
    expect(formatRemaining(90_000)).toBe("2 min");
    expect(formatRemaining(45_500)).toBe("46 s");
    expect(maintenanceFor(st, "node-12", NOW + 90_000)).toBeUndefined();
    expect(maintenanceFor(st, "node-07", NOW)).toBeUndefined();
    expect(maintenanceFor(undefined, "node-12", NOW)).toBeUndefined();
  });
});

// Red team: the device row let maintenance win over everything, so a node that
// went offline (or faulted) during maintenance showed a calm "maintenance".
describe("device tone", () => {
  test("maintenance replaces healthy only; offline, unknown and faults stay", () => {
    expect(deviceTone(HealthStatus.HEALTHY, "fresh", false, true)).toEqual([
      "maintenance",
      "maintenance",
    ]);
    expect(deviceTone(HealthStatus.MAINTENANCE, "fresh", true, true)[1]).toBe("maintenance");
    expect(deviceTone(HealthStatus.OFFLINE, "stale", true, true)).toEqual([
      "offline (in maintenance)",
      "unknown",
    ]);
    expect(deviceTone(HealthStatus.FAULTED, "fresh", false, true)[1]).toBe("fault");
    expect(deviceTone(undefined, "unknown", false, true)[1]).toBe("unknown");
    expect(deviceTone(undefined, "stale", false, true)[1]).toBe("stale");
  });
  test("outside maintenance an open lid is a fault, never healthy", () => {
    expect(deviceTone(HealthStatus.HEALTHY, "fresh", true, false)[1]).toBe("fault");
    expect(deviceTone(HealthStatus.OFFLINE, "stale", true, false)[1]).toBe("fault");
    expect(deviceTone(undefined, "fresh", false, false)).toEqual(["reporting", "fresh"]);
    expect(deviceTone(999 as HealthStatus, "fresh", false, false)).toEqual(["unknown", "unknown"]);
  });
});

// ADR 0019 amendment 2026-11-22: console-lock refusals are a diagnostic, and the
// hub's DEGRADED verdict for repeated ones never renders as success.
describe("USB console lock", () => {
  test("a refusal reads as a diagnostic, not an alarm", () => {
    expect(diagnosticLabel("console.locked", 3n)).toBe(
      "USB console: 3 command(s) refused, lid closed",
    );
    expect(diagnosticLabel("user.button", 2n)).toBe("USER: commissioning requested");
    expect(diagnosticLabel("module.fault", 4n)).toBe("module.fault 4");
  });
  test("DEGRADED (USB console access attempts while closed) is never healthy", () => {
    const [label, tone] = deviceTone(HealthStatus.DEGRADED, "fresh", false, false);
    expect(label).toBe("degraded");
    expect(tone).not.toBe("fresh");
    expect(tone).not.toBe("fault");
  });
});

// TL-SN-002's other tamper causes (ADR 0019 amendment 2026-11-22): battery door,
// mount, door magnet. Same rules as the enclosure: unknown is never closed.
describe("other tamper causes", () => {
  const node = create(PropertySchema, {
    id: "house-01",
    devices: [
      {
        id: "node-12",
        zoneId: "hallway",
        capabilities: ["tamper.enclosure", "tamper.battery_door", "tamper.mount", "tamper.magnet"],
      },
      { id: "node-07", zoneId: "backyard", capabilities: ["tamper.enclosure"] },
    ],
  });
  const cap = (capabilityId: string, open: boolean, freshness = Freshness.FRESH) => ({
    ref: { deviceId: "node-12", capabilityId },
    freshness,
    value: { kind: { case: "tamper" as const, value: { open } } },
  });

  test("declared causes are listed in order and start unknown", () => {
    const c = otherTampers(node, state({ capabilities: [] }), "node-12");
    expect(c.map((x) => x.capability)).toEqual([
      "tamper.battery_door",
      "tamper.mount",
      "tamper.magnet",
    ]);
    expect(c.every((x) => x.status.display === "unknown")).toBe(true);
    expect(causeLabel(c[0]!)).toBe("battery door unknown");
    expect(otherTampers(node, undefined, "node-07")).toEqual([]);
  });

  test("an open battery door is a named alarm and a device fault", () => {
    const st = state({ capabilities: [cap("tamper.battery_door", true, Freshness.STALE)] });
    const c = otherTampers(node, st, "node-12");
    expect(c[0]!.status.display).toBe("open");
    expect(causeLabel(c[0]!)).toBe("TAMPER: battery door open");
    expect(deviceHealth(st, "node-12")).toBe("fault");
  });

  test("a stale normal report is unknown, never normal", () => {
    const st = state({
      capabilities: [cap("tamper.magnet", false, Freshness.STALE), cap("tamper.mount", false)],
    });
    const c = otherTampers(node, st, "node-12");
    expect(causeLabel(c[1]!)).toBe("mount in place");
    expect(causeLabel(c[2]!)).toBe("door magnet unknown (last: normal)");
  });
});

describe("cameras (ADR 0020)", () => {
  const camProperty = create(PropertySchema, {
    id: "house-01",
    devices: [
      { id: "cam_patio", capabilities: ["camera.stream", "tamper.enclosure"] },
      { id: "cam_patio_video", hostDeviceId: "cam_patio", capabilities: ["camera.detection"] },
      { id: "node-07", capabilities: ["motion.detect"] },
    ],
  });
  const stream = (up: boolean, freshness: Freshness, throttled = 0) =>
    create(StateResponseSchema, {
      capabilities: [
        {
          ref: { deviceId: "cam_patio", capabilityId: "camera.stream" },
          freshness,
          value: {
            kind: {
              case: "streamStatus",
              value: {
                up,
                width: 1920,
                height: 1080,
                fpsMilli: 15000,
                bitrateKbps: 4000,
                socMilliC: 61000,
                throttled,
              },
            },
          },
        },
      ],
    });

  test("groups a Vision Node with the views that name it as host", () => {
    const g = cameraGroups(camProperty);
    expect(g).toHaveLength(1);
    expect(g[0]!.host.id).toBe("cam_patio");
    expect(g[0]!.views.map((d) => d.id)).toEqual(["cam_patio_video"]);
  });

  test("a stream is streaming only while fresh and up; stale or missing is unknown", () => {
    expect(streamView(stream(true, Freshness.FRESH), "cam_patio")).toMatchObject({
      display: "streaming",
    });
    expect(streamView(stream(true, Freshness.FRESH), "cam_patio").label).toContain("1920×1080");
    expect(streamView(stream(false, Freshness.FRESH), "cam_patio").display).toBe("down");
    expect(streamView(stream(true, Freshness.STALE), "cam_patio").display).toBe("unknown");
    expect(streamView(undefined, "cam_patio").display).toBe("unknown");
    expect(streamView(stream(true, Freshness.FRESH, 0x4), "cam_patio").label).toContain(
      "THROTTLED",
    );
  });

  test("recent detections come newest first with their media reference", () => {
    const det = (seq: number, device: string, label: string, media?: string) =>
      create(JournalEntrySchema, {
        journalSeq: BigInt(seq),
        receivedAtUnixMs: BigInt(1_800_000_000_000 + seq),
        event: {
          case: "observation",
          value: {
            envelope: {
              source: { deviceId: device, capabilityId: "camera.detection" },
              eventType: "camera.object_detected",
              payload: {
                kind: {
                  case: "objectDetected",
                  value: {
                    label,
                    confidencePermille: 870,
                    media: media
                      ? { store: "frigate", mediaId: media, hasSnapshot: true, hasClip: true }
                      : undefined,
                  },
                },
              },
            },
          },
        },
      });
    const out = recentDetections(
      [
        det(1, "cam_patio_video", "person", "e1"),
        det(2, "other", "car"),
        det(3, "cam_patio_video", "dog"),
      ],
      ["cam_patio_video"],
    );
    expect(out.map((d) => d.label)).toEqual(["dog", "person"]);
    expect(out[1]).toMatchObject({ mediaId: "e1", hasSnapshot: true, confidencePercent: 87 });
    expect(out[0]!.mediaId).toBeUndefined();
  });
});
