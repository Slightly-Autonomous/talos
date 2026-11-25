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

  test("refuses commands past expiresAtUnixMs and never acts on them", async () => {
    const now = 1_800_000_000_000;
    const pending = {
      "lock-3": [
        { id: "late", expiresAtUnixMs: String(now - 1), args: { setLocked: { locked: true } } },
        { id: "edge", expiresAtUnixMs: String(now), args: { setLocked: { locked: true } } },
      ],
      patio_speaker: [
        { id: "ok", expiresAtUnixMs: String(now + 5000), args: { audioSpeak: { text: "hi" } } },
      ],
    };
    const hub = fakeHub(pending);
    const s = new DevSim({
      hubUrl: "http://hub",
      devices: DEV_DEVICES,
      fetch: hub.fetch,
      speakerLatencyMs: 0,
      lockTravelMs: 0,
      nowMs: () => now,
    });
    await s.pollOnce();
    const acks = hub.calls.filter((c) => c.path === "/v1/devices/acks").map((c) => c.body);
    expect(acks).toContainEqual({ commandId: "late", accepted: false, detail: "expired" });
    expect(acks).toContainEqual({ commandId: "edge", accepted: false, detail: "expired" });
    expect(acks).toContainEqual({ commandId: "ok", accepted: true, detail: "playing" });
    expect(s.status().devices["lock-3"]!.state.locked).toBeUndefined();
    expect(observations(hub.calls).map((c) => c.body!.eventType)).toEqual([
      "audio.playback_started",
    ]);
  });

  test("keeps a base-path prefix and reports non-JSON hub errors", async () => {
    const paths: string[] = [];
    const s = new DevSim({
      hubUrl: "http://hub/talos",
      devices: DEV_DEVICES,
      fetch: (async (input: string | URL) => {
        paths.push(new URL(input.toString()).pathname);
        return new Response("<html>bad gateway</html>", { status: 502 });
      }) as typeof fetch,
    });
    await expect(s.heartbeat("node-07")).rejects.toThrow(/502/);
    expect(paths).toEqual(["/talos/v1/devices/observations"]);
  });
  test("node-12 reports its enclosure tamper loop device-level, with a boot state report", async () => {
    const { s, calls } = sim();
    await s.boot();
    const boot = observations(calls).find((c) => c.body!.eventType === "tamper.state");
    expect(boot?.token).toBe("dev-node-12-token");
    expect(boot?.body).toMatchObject({
      source: { deviceId: "node-12", capabilityId: "tamper.enclosure" },
      payload: { tamper: { open: false } },
    });
    expect((boot?.body?.source as Record<string, unknown>).slot).toBeUndefined();
    await s.tamper("node-12", true);
    await s.tamper("node-12", false);
    expect(
      observations(calls)
        .filter(
          (c) =>
            c.token === "dev-node-12-token" &&
            (c.body!.source as Record<string, unknown>).capabilityId === "tamper.enclosure",
        )
        .map((c) => c.body!.eventType),
    ).toEqual(["tamper.state", "tamper.opened", "tamper.closed"]);
    await expect(s.tamper("node-07", true)).rejects.toThrow("no tamper loop");
  });
  test("node-12 is a TL-SN-002: built-in contact in slot 0, AA heartbeat, USER test report", async () => {
    const { s, calls } = sim();
    await s.boot();
    const state = observations(calls).find(
      (c) => c.body!.eventType === "contact.state" && c.token === "dev-node-12-token",
    );
    expect(state?.body).toMatchObject({
      source: { deviceId: "node-12", slot: 0, capabilityId: "contact.open_state" },
    });
    const beat = observations(calls).find(
      (c) => c.body!.eventType === "device.heartbeat" && c.token === "dev-node-12-token",
    );
    expect(beat?.body).toMatchObject({ payload: { heartbeat: { batteryMv: 3000 } } });
    const before = observations(calls).length;
    await s.userButton("node-12");
    const after = observations(calls).slice(before);
    expect(after.map((c) => c.body!.eventType)).toEqual(["device.heartbeat"]);
    await expect(s.userButton("node-07")).rejects.toThrow("no USER button");
  });
});

describe("Vision Node and recorder (ADR 0020)", () => {
  test("boot reports the stream, lid and heartbeat; the stream can go down", async () => {
    const { s, calls } = sim();
    await s.boot();
    const cam = observations(calls).filter((c) => c.token === "dev-cam-patio-token");
    expect(cam.map((c) => c.body!.eventType)).toEqual([
      "tamper.state",
      "camera.stream_status",
      "device.heartbeat",
    ]);
    expect(cam[1]!.body).toMatchObject({
      source: { deviceId: "cam_patio", capabilityId: "camera.stream" },
      payload: { streamStatus: { up: true, width: 1920 } },
    });
    await s.stream("cam_patio", false);
    expect(observations(calls).at(-1)!.body).toMatchObject({
      payload: { streamStatus: { up: false } },
    });
    await expect(s.stream("node-07", false)).rejects.toThrow(/Vision Node/);
  });

  test("detections carry a media id the fake recorder then serves", async () => {
    const { s, calls } = sim();
    const id = await s.detect("cam_patio_video", "person");
    expect(s.media.has(id)).toBe(true);
    expect(observations(calls).at(-1)!.body).toMatchObject({
      eventType: "camera.object_detected",
      source: { deviceId: "cam_patio_video", capabilityId: "camera.detection" },
      payload: { objectDetected: { label: "person", media: { mediaId: id, hasSnapshot: true } } },
    });
  });

  test("a bookmark is ACKed, then its clip is reported with the command id", async () => {
    const pending = {
      cam_patio_video: [
        { id: "cmd:r:e1", operation: "camera.bookmark", args: { bookmark: { durationS: 30 } } },
      ],
    };
    const hub = fakeHub(pending);
    const s = new DevSim({ hubUrl: "http://hub", devices: DEV_DEVICES, fetch: hub.fetch, clipDelayMs: 0 });
    await s.pollOnce();
    const acks = hub.calls.filter((c) => c.path === "/v1/devices/acks");
    expect(acks[0]!.body).toMatchObject({ commandId: "cmd:r:e1", accepted: true });
    expect(observations(hub.calls).at(-1)!.body).toMatchObject({
      source: { deviceId: "cam_patio_video", capabilityId: "camera.recording" },
      eventType: "camera.clip_recorded",
      payload: { clipRecorded: { commandId: "cmd:r:e1" } },
    });
  });
});
