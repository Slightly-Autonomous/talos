// One sync pass: copy new journal entries and the current read model from the
// hub API into Supabase. Idempotent (every write is an upsert keyed by stable
// ids) and resumable (cursor stored in Supabase), so retries and restarts are
// safe. It only ever READS from the hub.
import type { HubClient } from "@talos/contracts/client";
import { capabilityRow, commandRow, journalRow, propertyStateRow } from "./rows.ts";

// The subset of supabase-js the worker uses, so tests can supply a fake.
export interface SupabaseLike {
  from(table: string): {
    upsert(rows: unknown[], opts: { onConflict: string }): PromiseLike<{ error: unknown }>;
    select(columns: string): {
      eq(
        column: string,
        value: string,
      ): {
        maybeSingle(): PromiseLike<{ data: { after_seq: number } | null; error: unknown }>;
      };
    };
  };
}

export type HubReader = Pick<HubClient, "journal" | "state">;

export interface SyncResult {
  journalRows: number;
  capabilityRows: number;
  commandRows: number;
  cursor: bigint;
}

async function check<T extends { error: unknown }>(p: PromiseLike<T>, what: string): Promise<T> {
  const r = await p;
  if (r.error) throw new Error(`${what}: ${JSON.stringify(r.error)}`);
  return r;
}

export async function syncOnce(
  hub: HubReader,
  db: SupabaseLike,
  propertyId: string,
  now = new Date(),
  pageSize = 500,
): Promise<SyncResult> {
  const cur = await check(
    db.from("talos_sync_cursor").select("after_seq").eq("property_id", propertyId).maybeSingle(),
    "read cursor",
  );
  let cursor = BigInt(cur.data?.after_seq ?? 0);
  let journalRows = 0;

  for (;;) {
    const page = await hub.journal(cursor, pageSize);
    if (page.entries.length > 0) {
      await check(
        db.from("talos_journal").upsert(
          page.entries.map((e) => journalRow(propertyId, e)),
          { onConflict: "property_id,journal_seq" },
        ),
        "upsert journal",
      );
      journalRows += page.entries.length;
      cursor = page.nextAfter;
      await check(
        db
          .from("talos_sync_cursor")
          .upsert(
            [{ property_id: propertyId, after_seq: Number(cursor), updated_at: now.toISOString() }],
            { onConflict: "property_id" },
          ),
        "advance cursor",
      );
    }
    if (!page.hasMore) break;
  }

  const state = await hub.state();
  const caps = state.capabilities.map((c) => capabilityRow(propertyId, c));
  const cmds = state.commands.map((c) => commandRow(propertyId, c));
  if (caps.length) {
    await check(
      db.from("talos_capability_state").upsert(caps, {
        onConflict: "property_id,device_id,slot,capability_id",
      }),
      "upsert capability state",
    );
  }
  if (cmds.length) {
    await check(
      db.from("talos_commands").upsert(cmds, { onConflict: "property_id,command_id" }),
      "upsert commands",
    );
  }
  await check(
    db.from("talos_property_state").upsert([propertyStateRow(state, now)], {
      onConflict: "property_id",
    }),
    "upsert property state",
  );
  return { journalRows, capabilityRows: caps.length, commandRows: cmds.length, cursor };
}
