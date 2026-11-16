// Pure helpers for the configuration editors (ADR 0012).
import { create, clone } from "@bufbuild/protobuf";
import {
  ConfigDocumentSchema,
  type ConfigDocument,
} from "@talos/contracts/talos/server/v1/config_pb";
import { VerificationMode } from "@talos/contracts/talos/v1/command_pb";
import type { Point, Property } from "@talos/contracts/talos/v1/property_pb";
import { ArmMode, RuleSchema, type Rule } from "@talos/contracts/talos/v1/rule_pb";

export function parseHhMm(s: string): number | undefined {
  const m = /^(\d{1,2}):(\d{2})$/.exec(s.trim());
  if (!m) return undefined;
  const h = Number(m[1]);
  const min = Number(m[2]);
  return h < 24 && min < 60 ? h * 60 + min : undefined;
}

export function formatHhMm(minute: number): string {
  return `${String(Math.floor(minute / 60)).padStart(2, "0")}:${String(minute % 60).padStart(2, "0")}`;
}

export function snap(v: number, step = 0.5): number {
  return Math.round(v / step) * step;
}

export function pointInPolygon(p: { x: number; y: number }, poly: Point[]): boolean {
  let inside = false;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const a = poly[i]!;
    const b = poly[j]!;
    if (a.y > p.y !== b.y > p.y && p.x < ((b.x - a.x) * (p.y - a.y)) / (b.y - a.y) + a.x) {
      inside = !inside;
    }
  }
  return inside;
}

export function zoneAt(property: Property, p: { x: number; y: number }): string | undefined {
  return property.zones.find((z) => z.polygon.length >= 3 && pointInPolygon(p, z.polygon))?.id;
}

export interface RuleDraft {
  id: string;
  enabled: boolean;
  triggerEventType: string;
  zoneId: string; // "" = any zone
  modes: ArmMode[];
  schedule: { start: string; end: string } | null;
  action: "speak" | "notify";
  targetDeviceId: string;
  text: string;
  volumePercent: number;
}

export function emptyDraft(): RuleDraft {
  return {
    id: "",
    enabled: true,
    triggerEventType: "motion.detected",
    zoneId: "",
    modes: [ArmMode.ARMED_AWAY],
    schedule: null,
    action: "speak",
    targetDeviceId: "",
    text: "",
    volumePercent: 65,
  };
}

export function draftFromRule(r: Rule): RuleDraft {
  const a = r.actions[0];
  const speak = a?.args?.kind.case === "audioSpeak" ? a.args.kind.value : undefined;
  const notify = a?.args?.kind.case === "notify" ? a.args.kind.value : undefined;
  return {
    id: r.id,
    enabled: r.enabled,
    triggerEventType: r.triggerEventType,
    zoneId: r.triggerZoneId ?? "",
    modes: [...r.armedModes],
    schedule: r.schedule
      ? { start: formatHhMm(r.schedule.startMinute), end: formatHhMm(r.schedule.endMinute) }
      : null,
    action: notify ? "notify" : "speak",
    targetDeviceId: a?.target?.deviceId ?? "",
    text: speak?.text ?? notify?.text ?? "",
    volumePercent: speak?.volumePercent ?? 65,
  };
}

export type DraftError = string;

// Builds a typed Rule. Speak actions verify by observing playback; notify is
// ack-only (no independent evidence a person read it).
export function ruleFromDraft(d: RuleDraft, propertyId: string, revision = 1): Rule | DraftError {
  if (!/^[a-z0-9_]+$/.test(d.id)) return "Rule id: lowercase letters, digits and _ only";
  if (!d.targetDeviceId) return "Choose a target device";
  if (!d.text.trim()) return "Enter the message text";
  let schedule;
  if (d.schedule) {
    const start = parseHhMm(d.schedule.start);
    const end = parseHhMm(d.schedule.end);
    if (start === undefined || end === undefined) return "Schedule times must be HH:MM";
    schedule = { startMinute: start, endMinute: end };
  }
  const target = {
    deviceId: d.targetDeviceId,
    capabilityId: d.action === "speak" ? "audio.speak" : "notify",
  };
  return create(RuleSchema, {
    id: d.id,
    revision,
    propertyId,
    principalId: `rule:${d.id}`,
    enabled: d.enabled,
    triggerEventType: d.triggerEventType,
    triggerZoneId: d.zoneId || undefined,
    armedModes: d.modes,
    schedule,
    actions: [
      d.action === "speak"
        ? {
            operation: "audio.speak",
            target,
            args: {
              kind: { case: "audioSpeak", value: { text: d.text, volumePercent: d.volumePercent } },
            },
            ttlMs: 5000n,
            verification: VerificationMode.OBSERVED_STATE,
            verificationWindowMs: 3000n,
            expect: {
              source: target,
              payload: { kind: { case: "audioPlayback", value: { playing: true } } },
            },
          }
        : {
            operation: "notify",
            target,
            args: { kind: { case: "notify", value: { recipientRole: "owner", text: d.text } } },
            ttlMs: 5000n,
            verification: VerificationMode.ACK_ONLY,
            verificationWindowMs: 3000n,
          },
    ],
  });
}

export function withRules(doc: ConfigDocument, rules: Rule[]): ConfigDocument {
  const next = clone(ConfigDocumentSchema, doc);
  next.rules = rules;
  return next;
}

export function withProperty(doc: ConfigDocument, property: Property): ConfigDocument {
  const next = clone(ConfigDocumentSchema, doc);
  next.property = property;
  return next;
}

// Short human summary of what a layout save changes.
export function layoutDiff(before: Property, after: Property): string[] {
  const out: string[] = [];
  const zb = new Map(before.zones.map((z) => [z.id, z]));
  for (const z of after.zones) {
    const old = zb.get(z.id);
    if (!old) out.push(`add zone ${z.id}`);
    else {
      if (old.name !== z.name) out.push(`rename ${z.id}: "${old.name}" → "${z.name}"`);
      if (JSON.stringify(old.polygon) !== JSON.stringify(z.polygon)) out.push(`reshape ${z.id}`);
    }
  }
  for (const z of before.zones)
    if (!after.zones.some((n) => n.id === z.id)) out.push(`remove zone ${z.id}`);
  const db = new Map(before.devices.map((d) => [d.id, d]));
  for (const d of after.devices) {
    const old = db.get(d.id);
    if (old && JSON.stringify(old.position) !== JSON.stringify(d.position))
      out.push(`move ${d.id}`);
    if (old && old.zoneId !== d.zoneId) out.push(`${d.id} now in ${d.zoneId}`);
  }
  const ab = new Map(before.assets.map((a) => [a.id, a]));
  for (const a of after.assets) {
    if (JSON.stringify(ab.get(a.id)?.segment) !== JSON.stringify(a.segment))
      out.push(`redraw ${a.id}`);
  }
  return out;
}
