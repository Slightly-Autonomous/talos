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
import { HealthStatus } from "@talos/contracts/talos/v1/health_pb";
import type { JournalEntry } from "@talos/contracts/talos/v1/journal_pb";
import type { Device, Property, Point } from "@talos/contracts/talos/v1/property_pb";
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

export type DeviceHealth = "fresh" | "stale" | "unknown" | "fault";

// An open enclosure (tamper) is a fault whatever else the device reports, and
// stays one while that report is stale: an open lid never ages into "healthy".
export function deviceHealth(state: StateResponse | undefined, deviceId: string): DeviceHealth {
  const caps = state?.capabilities.filter((c) => c.ref?.deviceId === deviceId) ?? [];
  if (caps.length === 0) return "unknown";
  if (caps.some((c) => c.value?.kind.case === "tamper" && c.value.kind.value.open)) return "fault";
  return caps.some((c) => c.freshness === Freshness.FRESH) ? "fresh" : "stale";
}

export const TAMPER_CAPABILITY = "tamper.enclosure";

export type TamperDisplay = "open" | "closed" | "unknown";

export interface TamperStatus {
  display: TamperDisplay;
  // Last reported value, when there is one (for "unknown (last: ...)").
  last: boolean | undefined;
}

// Enclosure tamper of one device (HD-032), or undefined if the device has no
// tamper loop. Never reported, or stale, is unknown, never closed. An open lid
// shows as open even when its report is stale: it is an alarm, not a reading.
export function tamperStatus(
  property: Property,
  state: StateResponse | undefined,
  deviceId: string,
): TamperStatus | undefined {
  const declared = property.devices
    .find((d) => d.id === deviceId)
    ?.capabilities.includes(TAMPER_CAPABILITY);
  const cap = state?.capabilities.find(
    (c) => c.ref?.deviceId === deviceId && c.ref.capabilityId === TAMPER_CAPABILITY,
  );
  if (!declared && !cap) return undefined;
  const last = cap?.value?.kind.case === "tamper" ? cap.value.kind.value.open : undefined;
  if (last === undefined) return { display: "unknown", last };
  if (last) return { display: "open", last };
  return { display: cap?.freshness === Freshness.FRESH ? "closed" : "unknown", last };
}

export interface MaintenanceView {
  untilMs: number;
  remainingMs: number;
  principalId: string;
}

// A node in maintenance (tamper alarm suppressed), or undefined. Uses the
// hub's window; a window already past `nowMs` is shown as over, even before
// the hub's next tick journals the end.
export function maintenanceFor(
  state: StateResponse | undefined,
  deviceId: string,
  nowMs: number,
): MaintenanceView | undefined {
  const m = state?.maintenance.find((x) => x.deviceId === deviceId);
  if (!m) return undefined;
  const untilMs = Number(m.untilUnixMs);
  if (untilMs <= nowMs) return undefined;
  return { untilMs, remainingMs: untilMs - nowMs, principalId: m.principalId };
}

export function formatRemaining(ms: number): string {
  if (ms >= 60_000) return `${Math.ceil(ms / 60_000)} min`;
  return `${Math.ceil(ms / 1000)} s`;
}

export type HealthTone = "fresh" | "stale" | "unknown" | "fault" | "maintenance";

const HEALTH_LABEL: Record<number, [string, HealthTone]> = {
  [HealthStatus.HEALTHY]: ["healthy", "fresh"],
  [HealthStatus.DEGRADED]: ["degraded", "stale"],
  [HealthStatus.OFFLINE]: ["offline", "unknown"],
  [HealthStatus.LOW_POWER]: ["low power", "stale"],
  [HealthStatus.FAULTED]: ["faulted", "fault"],
  [HealthStatus.MAINTENANCE]: ["maintenance", "maintenance"],
};

// Label and dot tone for a device row. The hub's projection wins when there is
// one; otherwise the local fallback. Maintenance covers the tamper alarm only:
// it replaces a "healthy"/"reporting" tone, never offline, unknown, stale or a
// fault (ADR 0011: missing data is unknown, never something calmer). Outside
// maintenance an open enclosure is a fault whatever the rest says.
export function deviceTone(
  projected: HealthStatus | undefined,
  fallback: DeviceHealth,
  tamperOpen: boolean,
  inMaintenance: boolean,
): [string, HealthTone] {
  const [label, tone]: [string, HealthTone] =
    projected !== undefined
      ? (HEALTH_LABEL[projected] ?? ["unknown", "unknown"])
      : [
          fallback === "fresh"
            ? "reporting"
            : fallback === "stale"
              ? "stale"
              : fallback === "fault"
                ? "faulted"
                : "no reports",
          fallback,
        ];
  if (inMaintenance) {
    if (tone === "fresh") return ["maintenance", "maintenance"];
    if (tone === "maintenance") return [label, tone];
    return [`${label} (in maintenance)`, tone];
  }
  if (tamperOpen && tone !== "fault") return [label, "fault"];
  return [label, tone];
}

// Device diagnostics (observability plane, ADR 0014) in words. "console.locked":
// a TL-SN-002 refused USB console commands because its lid was closed (ADR 0019
// amendment 2026-11-22). A diagnostic, never an alarm; the hub's health turns
// repeated refusals into DEGRADED evidence, which the device row shows.
export function diagnosticLabel(kind: string, value: bigint | number): string {
  switch (kind) {
    case "console.locked":
      return `USB console: ${String(value)} command(s) refused, lid closed`;
    case "user.button":
      return Number(value) === 2 ? "USER: commissioning requested" : "USER: test report";
    default:
      return `${kind} ${String(value)}`;
  }
}

export function tamperLabel(t: TamperStatus): string {
  if (t.display === "open") return "TAMPER: enclosure open";
  if (t.display === "closed") return "enclosure closed";
  return t.last === undefined ? "enclosure unknown" : "enclosure unknown (last: closed)";
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
    case "maintenanceChanged": {
      const m = e.event.value;
      return m.untilUnixMs !== undefined
        ? `maintenance of ${m.deviceId} by ${m.principalId} until ${new Date(Number(m.untilUnixMs)).toLocaleTimeString()}`
        : `maintenance of ${m.deviceId} ${m.reason} (${m.principalId})`;
    }
    case "alarmSuppressed":
      return `tamper alarm for ${e.event.value.deviceId} suppressed (maintenance by ${e.event.value.maintenancePrincipalId})`;
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
    case "tamper":
      return v.value.open ? "TAMPER: open" : "enclosure closed";
    default:
      return "—";
  }
}

// --- Device sensors: built-in (TL-SN-002) vs slots ---------------------------------------
//
// TL-SN-002 (ADR 0019) has its sensors soldered on, so its page lists built-in
// sensors, never slots or cards. The table mirrors
// contracts/manifests/boards/sensor_node_v2.yaml (a vitest holds the two together).
// Configuration only: it says what is fitted, never that a sensor is working.
export const BUILT_IN_BOARDS: Record<string, readonly string[]> = {
  sensor_node_v2: [
    "tmag5273a2_magnetic",
    "vibration_lis2dw12_v1",
    "pir223d_motion",
    "environment_sht40_v1",
  ],
};

const MODULE_LABELS: Record<string, string> = {
  tmag5273a2_magnetic: "door contact (magnetic)",
  vibration_lis2dw12_v1: "vibration",
  pir223d_motion: "motion (PIR)",
  environment_sht40_v1: "temperature / humidity",
  pir_ekmb_v1: "motion (PIR)",
  reed_contact_v1: "door contact (reed)",
  smart_camera_s3_v1: "camera",
  tl_mod_001_magnetic: "door contact (TL-MOD-001)",
  tl_mod_002_pir: "motion (TL-MOD-002)",
  tl_mod_003_imu: "vibration (TL-MOD-003)",
  tl_mod_004_environment: "temperature / humidity (TL-MOD-004)",
};

export interface SensorItem {
  index: number;
  module: string;
  label: string;
}
export interface DeviceSensors {
  kind: "built-in" | "slots";
  items: SensorItem[];
}

export function moduleLabel(module: string): string {
  return MODULE_LABELS[module] ?? module;
}

export function deviceSensors(
  device: Pick<Device, "boardId" | "slotModules">,
): DeviceSensors | undefined {
  const builtIn = device.boardId ? BUILT_IN_BOARDS[device.boardId] : undefined;
  if (builtIn) {
    return {
      kind: "built-in",
      items: builtIn.map((module, index) => ({ index, module, label: moduleLabel(module) })),
    };
  }
  const items = device.slotModules
    .map((module, index) => ({ index, module, label: moduleLabel(module) }))
    .filter((i) => i.module !== "");
  return items.length ? { kind: "slots", items } : undefined;
}

export function sensorsLabel(s: DeviceSensors): string {
  if (s.kind === "built-in") return `built-in: ${s.items.map((i) => i.label).join(", ")}`;
  return `slots: ${s.items.map((i) => `${i.index} ${i.label}`).join(", ")}`;
}
