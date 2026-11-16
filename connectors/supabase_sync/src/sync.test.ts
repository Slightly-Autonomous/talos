import { create } from "@bufbuild/protobuf";
import { describe, expect, test } from "vitest";
import {
  CommandOutcome,
  Freshness,
  JournalPageSchema,
  StateResponseSchema,
} from "@talos/contracts/talos/api/v1/api_pb";
import { CommandStatus } from "@talos/contracts/talos/v1/command_pb";
import { JournalEntrySchema } from "@talos/contracts/talos/v1/journal_pb";
import { ArmMode } from "@talos/contracts/talos/v1/rule_pb";
import { syncOnce, type HubReader, type SupabaseLike } from "./sync.ts";

class FakeSupabase implements SupabaseLike {
  tables = new Map<string, Map<string, Record<string, unknown>>>();
  failNext: string | null = null;

  table(name: string) {
    if (!this.tables.has(name)) this.tables.set(name, new Map());
    return this.tables.get(name)!;
  }
  from(name: string) {
    return {
      upsert: async (rows: unknown[], opts: { onConflict: string }) => {
        if (this.failNext === name) {
          this.failNext = null;
          return { error: { message: "boom" } };
        }
        const keys = opts.onConflict.split(",");
        for (const row of rows as Record<string, unknown>[]) {
          this.table(name).set(keys.map((k) => String(row[k])).join("|"), row);
        }
        return { error: null };
      },
      select: () => ({
        eq: (_c: string, v: string) => ({
          maybeSingle: async () => ({
            data: (this.table(name).get(v) as { after_seq: number } | undefined) ?? null,
            error: null,
          }),
        }),
      }),
    };
  }
}

function fakeHub(total: number): HubReader {
  const entries = Array.from({ length: total }, (_, i) =>
    create(JournalEntrySchema, {
      journalSeq: BigInt(i + 1),
      receivedAtUnixMs: BigInt(1_800_000_000_000 + i),
      event: { case: "armModeChanged", value: { mode: ArmMode.ARMED_AWAY, principalId: "owner" } },
    }),
  );
  return {
    journal: async (after: bigint, limit = 100) => {
      const slice = entries.filter((e) => e.journalSeq > after).slice(0, limit);
      return create(JournalPageSchema, {
        entries: slice,
        nextAfter: slice.length ? slice[slice.length - 1]!.journalSeq : after,
        hasMore: entries.filter((e) => e.journalSeq > after).length > limit,
      });
    },
    state: async () =>
      create(StateResponseSchema, {
        propertyId: "house-01",
        throughSeq: BigInt(total),
        armMode: ArmMode.ARMED_AWAY,
        capabilities: [
          {
            ref: { deviceId: "node-07", slot: 0, capabilityId: "motion.detect" },
            value: { kind: { case: "motion", value: { detected: true } } },
            eventType: "motion.detected",
            freshness: Freshness.FRESH,
            receivedAtUnixMs: 1_800_000_000_000n,
            journalSeq: 2n,
          },
        ],
        commands: [
          {
            command: { id: "cmd-1", operation: "audio.speak" },
            status: CommandStatus.VERIFIED,
            outcome: CommandOutcome.VERIFIED,
          },
        ],
      }),
  };
}

describe("syncOnce", () => {
  test("copies journal in pages and the read model, advancing the cursor", async () => {
    const db = new FakeSupabase();
    const r = await syncOnce(fakeHub(7), db, "house-01", new Date(0), 3);
    expect(r.journalRows).toBe(7);
    expect(r.cursor).toBe(7n);
    expect(db.table("talos_journal").size).toBe(7);
    expect(db.table("talos_sync_cursor").get("house-01")).toMatchObject({ after_seq: 7 });
    const cap = [...db.table("talos_capability_state").values()][0];
    expect(cap).toMatchObject({ device_id: "node-07", slot: 0, freshness: "FRESHNESS_FRESH" });
    const cmd = [...db.table("talos_commands").values()][0];
    expect(cmd).toMatchObject({
      status: "COMMAND_STATUS_VERIFIED",
      outcome: "COMMAND_OUTCOME_VERIFIED",
    });
    expect(db.table("talos_property_state").get("house-01")).toMatchObject({
      arm_mode: "ARM_MODE_ARMED_AWAY",
      through_seq: 7,
    });
  });

  test("is idempotent and resumes from the stored cursor", async () => {
    const db = new FakeSupabase();
    await syncOnce(fakeHub(4), db, "house-01");
    const again = await syncOnce(fakeHub(4), db, "house-01");
    expect(again.journalRows).toBe(0);
    expect(db.table("talos_journal").size).toBe(4);
    const more = await syncOnce(fakeHub(6), db, "house-01");
    expect(more.journalRows).toBe(2);
    expect(db.table("talos_journal").size).toBe(6);
  });

  test("a failed write throws without advancing the cursor past unsynced entries", async () => {
    const db = new FakeSupabase();
    db.failNext = "talos_journal";
    await expect(syncOnce(fakeHub(3), db, "house-01")).rejects.toThrow(/upsert journal/);
    expect(db.table("talos_sync_cursor").size).toBe(0);
    const retry = await syncOnce(fakeHub(3), db, "house-01");
    expect(retry.journalRows).toBe(3);
  });
});
