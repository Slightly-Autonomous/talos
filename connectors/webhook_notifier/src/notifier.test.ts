import { expect, test } from "vitest";
import { Notifier } from "./notifier.ts";

function harness(commands: unknown[], webhookStatus = 200) {
  const calls: { url: string; body: string; headers: Headers }[] = [];
  const f = (async (input: string | URL, init?: RequestInit) => {
    const url = input.toString();
    calls.push({ url, body: String(init?.body ?? ""), headers: new Headers(init?.headers) });
    if (url.endsWith("/v1/devices/commands")) return Response.json({ commands });
    if (url.startsWith("https://ntfy.example")) return new Response("", { status: webhookStatus });
    return Response.json({ status: "accepted" });
  }) as typeof fetch;
  const n = new Notifier({
    hubUrl: "http://hub",
    deviceToken: "dev-notifier",
    webhookUrl: "https://ntfy.example/talos",
    format: "ntfy",
    fetch: f,
  });
  return { n, calls };
}

const notify = {
  id: "c1",
  operation: "notify",
  args: { notify: { recipientRole: "owner", text: "Garage door opened" } },
};

test("delivers notify to the webhook once and acks success", async () => {
  const { n, calls } = harness([notify, notify]);
  expect(await n.processOnce()).toBe(1);
  const hook = calls.filter((c) => c.url.startsWith("https://ntfy.example"));
  expect(hook).toHaveLength(1);
  expect(hook[0]!.body).toBe("Garage door opened");
  const ack = calls.find((c) => c.url.endsWith("/v1/devices/acks"))!;
  expect(JSON.parse(ack.body)).toMatchObject({ commandId: "c1", accepted: true });
  expect(ack.headers.get("Authorization")).toBe("Bearer dev-notifier");
});

test("webhook failure is reported as a rejected command", async () => {
  const { n, calls } = harness([notify], 503);
  await n.processOnce();
  const ack = calls.find((c) => c.url.endsWith("/v1/devices/acks"))!;
  expect(JSON.parse(ack.body)).toMatchObject({ accepted: false, detail: "webhook 503" });
});

test("unsupported commands are refused, not silently dropped", async () => {
  const { n, calls } = harness([{ id: "x", operation: "audio.speak" }]);
  await n.processOnce();
  expect(JSON.parse(calls.find((c) => c.url.endsWith("/acks"))!.body)).toMatchObject({
    accepted: false,
  });
});
