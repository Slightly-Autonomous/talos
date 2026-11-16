// Supabase sync worker entry point. Configure with environment variables only:
//   HUB_URL, HUB_TOKEN (a read-only principal), PROPERTY_ID,
//   SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY, SYNC_INTERVAL_MS (default 5000)
import { createClient } from "@supabase/supabase-js";
import { HubClient } from "@talos/contracts/client";
import { syncOnce, type SupabaseLike } from "./sync.ts";

function env(name: string): string {
  const v = process.env[name];
  if (!v) throw new Error(`missing environment variable ${name}`);
  return v;
}

const hub = new HubClient({ baseUrl: env("HUB_URL"), token: env("HUB_TOKEN") });
const db = createClient(env("SUPABASE_URL"), env("SUPABASE_SERVICE_ROLE_KEY"), {
  auth: { persistSession: false },
}) as unknown as SupabaseLike;
const propertyId = env("PROPERTY_ID");
const interval = Number(process.env.SYNC_INTERVAL_MS ?? 5000);

let backoff = interval;
for (;;) {
  try {
    const r = await syncOnce(hub, db, propertyId);
    console.log(
      `synced journal+${r.journalRows} caps=${r.capabilityRows} commands=${r.commandRows} cursor=${r.cursor}`,
    );
    backoff = interval;
  } catch (err) {
    // The hub keeps working regardless; the cloud copy just lags.
    console.error(`sync failed (retrying in ${backoff} ms):`, err);
    backoff = Math.min(backoff * 2, 5 * 60_000);
  }
  await new Promise((r) => setTimeout(r, backoff));
}
