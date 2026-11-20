// Pure view-model functions: hub API messages -> what the UI shows.
// Kept free of React so they can be unit-tested, and conservative by design:
// missing or stale data is shown as unknown, never as "clear" or "healthy".
import type {
  CapabilityView,
  CommandView,
  StateResponse,
} from "@talos/contracts/talos/api/v1/api_pb";
import { clone } from "@bufbuild/protobuf";
import {
  CommandOutcome,
  Freshness,
  StateResponseSchema,
} from "@talos/contracts/talos/api/v1/api_pb";
import type { JournalEntry } from "@talos/contracts/talos/v1/journal_pb";
import type { Property, Point } from "@talos/contracts/talos/v1/property_pb";
import { ArmMode } from "@talos/contracts/talos/v1/rule_pb";

export const ACTIVE_WINDOW_MS = 30_000;

// The hub computes freshness when it answers. The client re-reads state every
// 5 s, so a snapshot older than this means several reads failed: its
// "fresh" can no longer be trusted.
export const STATE_MAX_AGE_MS = 15_000;

// Ages a state snapshot locally. If the last successful fetch is older than
// STATE_MAX_AGE_MS, or the hub is unreachable, every capability becomes
// UNKNOWN (the last value is kept, for "unknown (last: ...)" labels).
export function ageState(
  state: StateResponse | undefined,
  fetchedAtMs: number | undefined,
  nowMs: number,
  reachable: boolean,
): StateResponse | undefined {
  if (!state) return undefined;
  if (reachable && fetchedAtMs !== undefined && nowMs - fetchedAtMs <= STATE_MAX_AGE_MS)
    return state;
  const aged = clone(StateResponseSchema, state);
  for (const cap of aged.capabilities) cap.freshness = Freshness.UNKNOWN;
  return aged;
}

// Structural ref shape so plain objects and protobuf messages both work.
export interface RefLike {
  deviceId: string;
  slot?: number;
  capabilityId: string;
}

export function refKey(ref: RefLike | undefined): string {
  if (!ref) return "";
  return `${ref.deviceId}/${ref.slot ?? "-"}/${ref.capabilityId}`;
}

// Mirrors core Property::zone_of: an attached asset's zone wins, else the
// device placement.
export function zoneOf(property: Property, ref: RefLike | undefined): string | undefined {
  if (!ref) return undefined;
  for (const asset of property.assets) {
    for (const att of asset.attached) {
      if (
        att.deviceId === ref.deviceId &&
        att.capabilityId === ref.capabilityId &&
        (att.slot === undefined || ref.slot === undefined || att.slot === ref.slot) &&
        asset.zoneId
      ) {
        return asset.zoneId;
      }
    }
  }
  return property.devices.find((d) => d.id === ref.deviceId)?.zoneId;
}

export type ZoneActivity = "active" | "recent" | "quiet" | "unknown";

// Region-level activity from motion capabilities. A PIR says "something moved
// in this region", never where exactly, so the whole zone is shaded.
// Every motion source counts: a zone is "quiet" only when all of its PIRs
// (declared in the property, or reporting) are fresh and clear. Any stale or
// never-reported PIR makes it "unknown" unless another one is active.
export function zoneActivity(
  property: Property,
  state: StateResponse | undefined,
  nowMs: number,
): Map<string, ZoneActivity> {
  const out = new Map<string, ZoneActivity>();
  for (const z of property.zones) out.set(z.id, "unknown");
  if (!state) return out;
  const sources = new Map<string, ZoneActivity[]>();
  const add = (zone: string, a: ZoneActivity) =>
    sources.set(zone, [...(sources.get(zone) ?? []), a]);
  const reporting = new Set<string>();
  for (const cap of state.capabilities) {
    if (cap.ref?.capabilityId !== "motion.detect") continue;
    reporting.add(cap.ref.deviceId);
    const zone = zoneOf(property, cap.ref);
    if (!zone) continue;
    const detected = cap.value?.kind.case === "motion" && cap.value.kind.value.detected;
    const age = nowMs - Number(cap.receivedAtUnixMs);
    let a: ZoneActivity;
    if (cap.freshness !== Freshness.FRESH) a = "unknown";
    else if (detected) a = "active";
    else if (age <= ACTIVE_WINDOW_MS) a = "recent";
    else a = "quiet";
    add(zone, a);
  }
  // A declared PIR that never reported is a source we know nothing about.
  for (const d of property.devices) {
    if (!d.capabilities.includes("motion.detect") || reporting.has(d.id)) continue;
    const zone = zoneOf(property, { deviceId: d.id, capabilityId: "motion.detect" });
    if (zone) add(zone, "unknown");
  }
  for (const [zone, list] of sources) out.set(zone, combineZone(list));
  return out;
}

// Positive evidence wins; then any unknown source; then recent; quiet only
// when every source is fresh and clear.
function combineZone(list: ZoneActivity[]): ZoneActivity {
  for (const a of ["active", "unknown", "recent"] as const) {
    if (list.includes(a)) return a;
  }
  return list.length > 0 ? "quiet" : "unknown";
}

export interface PortalStatus {
  // Last reported values; check the *Stale flags before showing them.
  open: boolean | undefined;
  locked: boolean | undefined;
  openStale: boolean;
  lockedStale: boolean;
  stale: boolean;
}

export function portalStatus(property: Property, state: StateResponse | undefined) {
  const caps = new Map(state?.capabilities.map((c) => [refKey(c.ref), c]) ?? []);
  const out = new Map<string, PortalStatus>();
  for (const asset of property.assets) {
    const s: PortalStatus = {
      open: undefined,
      locked: undefined,
      openStale: false,
      lockedStale: false,
      stale: false,
    };
    for (const att of asset.attached) {
      const cap = caps.get(refKey(att));
      if (!cap) continue;
      const stale = cap.freshness !== Freshness.FRESH;
      if (stale) s.stale = true;
      if (cap.value?.kind.case === "contact") {
        s.open = cap.value.kind.value.open;
        s.openStale = stale;
      }
      if (cap.value?.kind.case === "lock") {
        s.locked = cap.value.kind.value.locked;
        s.lockedStale = stale;
      }
    }
    out.set(asset.id, s);
  }
  return out;
}

export type PortalDisplay = "open" | "closed" | "unknown";

// What the plan shows for the door itself. Stale is unknown, never closed.
export function portalDisplay(st: PortalStatus | undefined): PortalDisplay {
  if (!st || st.open === undefined || st.openStale) return "unknown";
  return st.open ? "open" : "closed";
}

// Locked styling only for a fresh lock report.
export function portalShowsLocked(st: PortalStatus | undefined): boolean {
  return !!st && st.locked === true && !st.lockedStale;
}

// Human label, e.g. "closed, locked" or "unknown (last: closed), lock unknown (last: locked)".
export function portalLabel(st: PortalStatus | undefined): string {
  const last = (v: boolean, yes: string, no: string) => `unknown (last: ${v ? yes : no})`;
  let door: string;
  if (!st || st.open === undefined) door = "unknown";
  else if (st.openStale) door = last(st.open, "open", "closed");
  else door = st.open ? "open" : "closed";
  if (!st || st.locked === undefined) return door;
  if (st.lockedStale) return `${door}, lock ${last(st.locked, "locked", "unlocked")}`;
  return `${door}, ${st.locked ? "locked" : "unlocked"}`;
}

export type DeviceHealth = "fresh" | "stale" | "unknown";

export function deviceHealth(state: StateResponse | undefined, deviceId: string): DeviceHealth {
  const caps = state?.capabilities.filter((c) => c.ref?.deviceId === deviceId) ?? [];
  if (caps.length === 0) return "unknown";
  return caps.some((c) => c.freshness === Freshness.FRESH) ? "fresh" : "stale";
}

export function armLabel(mode: ArmMode | undefined): string {
  switch (mode) {
    case ArmMode.ARMED_AWAY:
      return "Armed away";
    case ArmMode.ARMED_HOME:
      return "Armed home";
    case ArmMode.DISARMED:
      return "Disarmed";
    default:
      return "Unknown";
  }
}

export interface OutcomeBadge {
  label: string;
  tone: "good" | "warn" | "bad" | "neutral";
}

// Only VERIFIED is presented as success (security invariant #6).
export function outcomeBadge(outcome: CommandOutcome): OutcomeBadge {
  switch (outcome) {
    case CommandOutcome.VERIFIED:
      return { label: "Verified", tone: "good" };
    case CommandOutcome.UNVERIFIED:
      return { label: "Unverified", tone: "warn" };
    case CommandOutcome.UNKNOWN:
      return { label: "Unknown result", tone: "warn" };
    case CommandOutcome.FAILED:
      return { label: "Failed", tone: "bad" };
    case CommandOutcome.PENDING:
      return { label: "Pending", tone: "neutral" };
    default:
      return { label: "Unknown", tone: "neutral" };
  }
}

export function sortedCommands(state: StateResponse | undefined): CommandView[] {
  return [...(state?.commands ?? [])].sort(
    (a, b) => Number(b.command?.createdAtUnixMs ?? 0n) - Number(a.command?.createdAtUnixMs ?? 0n),
  );
}

// Motion detections per zone in [fromMs, toMs]: regional evidence counts.
export function motionHeat(
  property: Property,
  entries: JournalEntry[],
  fromMs: number,
  toMs: number,
): Map<string, number> {
  const heat = new Map<string, number>();
  for (const e of entries) {
    if (e.event.case !== "observation") continue;
    const t = Number(e.receivedAtUnixMs);
    if (t < fromMs || t > toMs) continue;
    const env = e.event.value.envelope;
    if (env?.eventType !== "motion.detected") continue;
    const zone = zoneOf(property, env.source);
    if (zone) heat.set(zone, (heat.get(zone) ?? 0) + 1);
  }
  return heat;
}

export function describeEntry(e: JournalEntry): string {
  switch (e.event.case) {
    case "observation": {
      const env = e.event.value.envelope;
      return `${env?.eventType ?? "observation"} from ${env?.source?.deviceId ?? "?"}`;
    }
    case "commandTransition": {
      const t = e.event.value;
      const status = commandStatusName(t.to);
      return `command ${t.commandId} → ${status}${t.reason ? ` (${t.reason})` : ""}${t.late ? " [late]" : ""}`;
    }
    case "armModeChanged":
      return `${armLabel(e.event.value.mode)} by ${e.event.value.principalId}`;
    case "sourceReset":
      return `sequence state of ${e.event.value.deviceId} reset by ${e.event.value.principalId}`;
    default:
      return "unknown entry";
  }
}

const STATUS_NAMES = [
  "unspecified",
  "requested",
  "denied",
  "accepted",
  "expired",
  "canceled",
  "dispatched",
  "device_acknowledged",
  "rejected",
  "completed_unverified",
  "verified",
  "unknown_result",
];
export function commandStatusName(status: number): string {
  return STATUS_NAMES[status] ?? `status_${status}`;
}

export interface Bounds {
  minX: number;
  minY: number;
  maxX: number;
  maxY: number;
}

export function bounds(property: Property): Bounds {
  const pts: Point[] = [
    ...property.zones.flatMap((z) => z.polygon),
    ...property.walls.flatMap((w) => [w.a, w.b].filter((p): p is Point => !!p)),
    ...property.devices.map((d) => d.position).filter((p): p is Point => !!p),
  ];
  if (pts.length === 0) return { minX: 0, minY: 0, maxX: 10, maxY: 10 };
  return {
    minX: Math.min(...pts.map((p) => p.x)),
    minY: Math.min(...pts.map((p) => p.y)),
    maxX: Math.max(...pts.map((p) => p.x)),
    maxY: Math.max(...pts.map((p) => p.y)),
  };
}

// SVG path for a coverage wedge. The plan is drawn with +y up, so callers
// flip y; angles here are in property coordinates.
export function wedgePath(
  cx: number,
  cy: number,
  headingDeg: number,
  halfDeg: number,
  range: number,
): string {
  const rad = (d: number) => (d * Math.PI) / 180;
  const a0 = rad(headingDeg - halfDeg);
  const a1 = rad(headingDeg + halfDeg);
  const p0 = { x: cx + range * Math.cos(a0), y: cy + range * Math.sin(a0) };
  const p1 = { x: cx + range * Math.cos(a1), y: cy + range * Math.sin(a1) };
  const large = halfDeg * 2 > 180 ? 1 : 0;
  return `M ${cx} ${cy} L ${p0.x} ${p0.y} A ${range} ${range} 0 ${large} 1 ${p1.x} ${p1.y} Z`;
}

export function capabilityLabel(c: CapabilityView): string {
  const v = c.value?.kind;
  switch (v?.case) {
    case "motion":
      return v.value.detected ? "motion" : "no motion";
    case "contact":
      return v.value.open ? "open" : "closed";
    case "lock":
      return v.value.locked ? "locked" : "unlocked";
    case "audioPlayback":
      return v.value.playing ? "playing" : "idle";
    case "heartbeat":
      return `${v.value.batteryMv} mV`;
    case "temperatureHumidity":
      return `${(v.value.temperatureMilliC / 1000).toFixed(1)} °C`;
    case "impact":
      return `impact ${v.value.peakMg} mg`;
    case "objectDetected":
      return `${v.value.label} (${(v.value.confidencePermille / 10).toFixed(0)}%)`;
    default:
      return "—";
  }
}
