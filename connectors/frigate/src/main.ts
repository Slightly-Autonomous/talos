// HUB_URL, FRIGATE_URL, PROPERTY_ID, CAMERA_MAP (JSON:
// {"backyard":{"deviceId":"node-07","token":"...","slot":2}}), POLL_MS
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
});
const poll = Number(process.env.POLL_MS ?? 2000);
for (;;) {
  await c.pollOnce().catch((e) => console.error("frigate:", e));
  await new Promise((r) => setTimeout(r, poll));
}
