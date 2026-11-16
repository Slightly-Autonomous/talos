// Maps hub API messages to Supabase rows. Supabase holds a read-only COPY of
// the hub's history and state (ADR 0010); the hub remains the authority.
import { enumToJson, toJson } from "@bufbuild/protobuf";
import {
  CommandOutcomeSchema,
  FreshnessSchema,
  type CapabilityView,
  type CommandView,
  type StateResponse,
} from "@talos/contracts/talos/api/v1/api_pb";
import { CommandStatusSchema } from "@talos/contracts/talos/v1/command_pb";
import { ObservationPayloadSchema } from "@talos/contracts/talos/v1/observation_pb";
import { JournalEntrySchema, type JournalEntry } from "@talos/contracts/talos/v1/journal_pb";
import { ArmModeSchema } from "@talos/contracts/talos/v1/rule_pb";
import { CommandViewSchema } from "@talos/contracts/talos/api/v1/api_pb";

const iso = (ms: bigint) => new Date(Number(ms)).toISOString();

export interface JournalRow {
  property_id: string;
  journal_seq: number;
  received_at: string;
  kind: string;
  entry: unknown;
}

export function journalRow(propertyId: string, e: JournalEntry): JournalRow {
  return {
    property_id: propertyId,
    journal_seq: Number(e.journalSeq),
    received_at: iso(e.receivedAtUnixMs),
    kind: e.event.case ?? "unknown",
    entry: toJson(JournalEntrySchema, e),
  };
}

export interface CapabilityRow {
  property_id: string;
  device_id: string;
  slot: number; // -1 when the capability has no slot
  capability_id: string;
  value: unknown;
  event_type: string;
  freshness: string;
  received_at: string;
  journal_seq: number;
}

export function capabilityRow(propertyId: string, c: CapabilityView): CapabilityRow {
  return {
    property_id: propertyId,
    device_id: c.ref?.deviceId ?? "",
    slot: c.ref?.slot ?? -1,
    capability_id: c.ref?.capabilityId ?? "",
    value: c.value ? toJson(ObservationPayloadSchema, c.value) : null,
    event_type: c.eventType,
    freshness: enumToJson(FreshnessSchema, c.freshness) as string,
    received_at: iso(c.receivedAtUnixMs),
    journal_seq: Number(c.journalSeq),
  };
}

export interface CommandRow {
  property_id: string;
  command_id: string;
  operation: string;
  status: string;
  outcome: string;
  view: unknown;
}

export function commandRow(propertyId: string, c: CommandView): CommandRow {
  return {
    property_id: propertyId,
    command_id: c.command?.id ?? "",
    operation: c.command?.operation ?? "",
    status: enumToJson(CommandStatusSchema, c.status) as string,
    outcome: enumToJson(CommandOutcomeSchema, c.outcome) as string,
    view: toJson(CommandViewSchema, c),
  };
}

export interface PropertyStateRow {
  property_id: string;
  arm_mode: string;
  through_seq: number;
  synced_at: string;
}

export function propertyStateRow(s: StateResponse, now: Date): PropertyStateRow {
  return {
    property_id: s.propertyId,
    arm_mode: enumToJson(ArmModeSchema, s.armMode) as string,
    through_seq: Number(s.throughSeq),
    synced_at: now.toISOString(),
  };
}
