import { describe, expect, test } from "vitest";
import { DevSim, DEV_DEVICES } from "./devsim.ts";

interface Call {
  path: string;
  token: string;
  body: Record<string, unknown> | undefined;
}

function fakeHub(pending: Record<string, unknown[]>) {
  const calls: Call[] = [];
  const f = (async (input: string | URL, init?: RequestInit) => {
    const url = new URL(input.toString());
    const token = new Headers(init?.headers).get("Authorization")?.slice(7) ?? "";
    const body = init?.body
      ? (JSON.parse(String(init.body)) as Record<string, unknown>)
      : undefined;
    calls.push({ path: url.pathname, token, body });
    if (url.pathname === "/v1/devices/commands") {
      const device = DEV_DEVICES.find((d) => d.token === token)!.id;
      const commands = pending[device] ?? [];
      pending[device] = [];
      return Response.json({ commands });
    }
    return Response.json({ status: "accepted" });
  }) as typeof fetch;
  return { calls, fetch: f };
}

function sim(pending: Record<string, unknown[]> = {}) {
  const hub = fakeHub(pending);
  const s = new DevSim({
    hubUrl: "http://hub",
    devices: DEV_DEVICES,
    fetch: hub.fetch,
    speakerLatencyMs: 0,
    lockTravelMs: 0,
    holdMs: 10,
    doorContactDevice: "node-12",
  });
  return { s, calls: hub.calls };
}

const observations = (calls: Call[]) => calls.filter((c) => c.path === "/v1/devices/observations");

describe("DevSim", () => {
  test("each device reports for itself with its own token and increasing sequence", async () => {
    const { s, calls } = sim();
    await s.motion("node-07");
    await s.motion("node-07"); // retrigger while active: no second detected
    const obs = observations(calls);
    expect(obs).toHaveLength(1);
    expect(obs[0]!.token).toBe("dev-node-07-token");
    expect(obs[0]!.body).toMatchObject({
      eventType: "motion.detected",
      source: { deviceId: "node-07", slot: 0, capabilityId: "motion.detect" },
      position: { sequence: "1" },
    });
    await new Promise((r) => setTimeout(r, 30));
    expect(observations(calls).map((c) => c.body!.eventType)).toEqual([
      "motion.detected",
      "motion.cleared",
    ]);
    s.stop();
  });

  test("speaker plays, reports playback and ACKs exactly once per command id", async () => {
    const cmd = { id: "cmd-1", args: { audioSpeak: { text: "hi" } } };
    const { s, calls } = sim({ patio_speaker: [cmd, cmd] });
    await s.pollOnce();
    const plays = observations(calls).filter((c) => c.body!.eventType === "audio.playback_started");
    expect(plays).toHaveLength(1);
    expect(calls.filter((c) => c.path === "/v1/devices/acks")).toHaveLength(1);
  });

  test("silent_ack fault ACKs without playing", async () => {
    const { s, calls } = sim({ patio_speaker: [{ id: "c", args: { audioSpeak: { text: "x" } } }] });
    s.setFault("patio_speaker", "silent_ack");
    await s.pollOnce();
    expect(observations(calls)).toHaveLength(0);
    expect(calls.filter((c) => c.path === "/v1/devices/acks")).toHaveLength(1);
  });

  test("lock refuses to bolt an open door and reports its real state", async () => {
    const { s, calls } = sim({ "lock-3": [{ id: "l1", args: { setLocked: { locked: true } } }] });
    await s.contact("node-12", true);
    await s.pollOnce();
    const lockState = observations(calls).find((c) => c.body!.eventType === "lock.state");
    expect(lockState!.body!.payload).toEqual({ lock: { locked: false } });
  });
});
