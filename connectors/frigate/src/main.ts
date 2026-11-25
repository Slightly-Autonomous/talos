// HUB_URL, FRIGATE_URL, PROPERTY_ID, CAMERA_MAP (JSON:
// {"patio":{"deviceId":"cam_patio_video","token":"..."}}), POLL_MS, HEARTBEAT_MS
import { FrigateConnector } from "./frigate.ts";

const env = (k: string) => {
  const v = process.env[k];
  if (!v) throw new Error(`missing ${k}`);
  return v;
};
const c = new FrigateConnector({
  hubUrl: env("HUB_URL"),
  frigateUrl: env("FRIGATE_URL"),
  propertyId: env("PROPERTY_ID"),
  cameras: JSON.parse(env("CAMERA_MAP")),
  heartbeatMs: Number(process.env.HEARTBEAT_MS ?? 60_000),
});
const poll = Number(process.env.POLL_MS ?? 2000);
for (;;) {
  for (const e of await c.tick()) console.error("frigate:", e);
  await new Promise((r) => setTimeout(r, poll));
}
