// HUB_URL, DEVICE_TOKEN (the notifier device's token), WEBHOOK_URL,
// WEBHOOK_FORMAT=ntfy|json, POLL_MS (default 1000)
import { Notifier } from "./notifier.ts";

const env = (k: string) => {
  const v = process.env[k];
  if (!v) throw new Error(`missing ${k}`);
  return v;
};
const n = new Notifier({
  hubUrl: env("HUB_URL"),
  deviceToken: env("DEVICE_TOKEN"),
  webhookUrl: env("WEBHOOK_URL"),
  format: process.env.WEBHOOK_FORMAT === "json" ? "json" : "ntfy",
});
const poll = Number(process.env.POLL_MS ?? 1000);
for (;;) {
  await n.processOnce().catch((e) => console.error("notifier:", e));
  await new Promise((r) => setTimeout(r, poll));
}
