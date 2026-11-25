import { expect, test } from "vitest";
import { FrigateConnector } from "./frigate.ts";

test("maps detections to camera observations once, per camera token", async () => {
  const posted: { body: Record<string, unknown>; auth: string | null }[] = [];
  const events = [
    { id: "e1", camera: "backyard", label: "person", start_time: 1800000000.5, top_score: 0.87 },
    {
      id: "e2",
      camera: "driveway",
      label: "car",
      start_time: 1800000001,
      data: { top_score: 0.6 },
    },
    { id: "e3", camera: "attic", label: "person", start_time: 1800000002, top_score: 0.9 },
  ];
  const f = (async (input: string | URL, init?: RequestInit) => {
    const url = input.toString();
    if (url.startsWith("http://frigate")) return Response.json(events);
    posted.push({
      body: JSON.parse(String(init?.body)),
      auth: new Headers(init?.headers).get("Authorization"),
    });
    return Response.json({ status: "accepted" });
  }) as typeof fetch;
  const c = new FrigateConnector({
    hubUrl: "http://hub",
    frigateUrl: "http://frigate:5000",
    propertyId: "house-01",
    cameras: {
      backyard: { deviceId: "node-07", token: "t-node07", slot: 2 },
      driveway: { deviceId: "camera_driveway", token: "t-drive" },
    },
    fetch: f,
  });
  expect(await c.pollOnce()).toBe(2); // attic is unmapped
  expect(await c.pollOnce()).toBe(0); // already seen
  expect(posted[0]!.auth).toBe("Bearer t-node07");
  expect(posted[0]!.body).toMatchObject({
    eventType: "camera.object_detected",
    source: { deviceId: "node-07", slot: 2, capabilityId: "camera.detection" },
    payload: { objectDetected: { label: "person", confidencePermille: 870 } },
  });
  expect(
    (posted[1]!.body.payload as { objectDetected: { confidencePermille: number } }).objectDetected
      .confidencePermille,
  ).toBe(600);
});

type Call = { url: string; method: string; body: Record<string, unknown> | null; auth: string | null };

// A scripted hub plus Frigate: returns what the routes say, records every call.
function harness(routes: Record<string, (body: Record<string, unknown> | null) => unknown>) {
  const calls: Call[] = [];
  const f = (async (input: string | URL, init?: RequestInit) => {
    const url = input.toString();
    const body = init?.body ? (JSON.parse(String(init.body)) as Record<string, unknown>) : null;
    calls.push({
      url,
      method: init?.method ?? "GET",
      body,
      auth: new Headers(init?.headers).get("Authorization"),
    });
    for (const [prefix, handler] of Object.entries(routes)) {
      if (url.startsWith(prefix)) {
        const out = handler(body);
        if (out instanceof Response) return out;
        return Response.json(out ?? { status: "accepted" });
      }
    }
    return Response.json({ status: "accepted" });
  }) as typeof fetch;
  return { calls, f };
}

const CAMS = { patio: { deviceId: "cam_patio_video", token: "t-video" } };

test("detections carry a media reference; an id the hub would refuse goes without one", async () => {
  const { calls, f } = harness({
    "http://frigate:5000/api/events?": () => [
      {
        id: "1800000000.5-ab12",
        camera: "patio",
        label: "person",
        start_time: 1800000000.5,
        has_clip: true,
        has_snapshot: true,
        top_score: 0.9,
      },
      { id: "../etc", camera: "patio", label: "car", start_time: 1800000001, top_score: 0.5 },
    ],
  });
  const c = new FrigateConnector({
    hubUrl: "http://hub",
    frigateUrl: "http://frigate:5000",
    propertyId: "house-01",
    cameras: CAMS,
    fetch: f,
  });
  expect(await c.pollOnce()).toBe(2);
  const posts = calls.filter((x) => x.url === "http://hub/v1/devices/observations");
  expect(posts[0]!.body).toMatchObject({
    eventId: "cam_patio_video:frigate:1800000000.5-ab12",
    payload: {
      objectDetected: {
        media: {
          store: "frigate",
          mediaId: "1800000000.5-ab12",
          startUnixMs: "1800000000500",
          hasClip: true,
          hasSnapshot: true,
        },
      },
    },
  });
  expect((posts[1]!.body!.payload as { objectDetected: object }).objectDetected).not.toHaveProperty(
    "media",
  );
});

test("heartbeats only while Frigate reports frames from the camera, at most once a period", async () => {
  let fps = 15;
  let t = 1_800_000_000_000;
  const { calls, f } = harness({
    "http://frigate:5000/api/stats": () => ({ cameras: { patio: { camera_fps: fps } } }),
  });
  const c = new FrigateConnector({
    hubUrl: "http://hub",
    frigateUrl: "http://frigate:5000",
    propertyId: "house-01",
    cameras: CAMS,
    fetch: f,
    now: () => t,
    heartbeatMs: 60_000,
  });
  expect(await c.heartbeatOnce()).toBe(1);
  expect(await c.heartbeatOnce()).toBe(0); // within the period
  t += 61_000;
  fps = 0; // Frigate up, camera feed broken: silence, so the hub sees it go stale
  expect(await c.heartbeatOnce()).toBe(0);
  fps = 12;
  expect(await c.heartbeatOnce()).toBe(1);
  const beats = calls.filter((x) => x.url === "http://hub/v1/devices/observations");
  expect(beats[0]!.body).toMatchObject({
    source: { deviceId: "cam_patio_video", capabilityId: "device.health" },
    eventType: "device.heartbeat",
  });
  expect(beats[0]!.auth).toBe("Bearer t-video");
});

test("a bookmark becomes a Frigate event, is ACKed, then reported as its clip", async () => {
  let ended = false;
  let t = 1_800_000_000_000;
  const { calls, f } = harness({
    "http://hub/v1/devices/commands": () => ({
      commands: [
        {
          id: "cmd:rule1:e1",
          operation: "camera.bookmark",
          args: { bookmark: { durationS: 20, label: "talos" } },
        },
      ],
    }),
    "http://frigate:5000/api/events/patio/talos/create": () => ({
      success: true,
      event_id: "1800000000.1-bm",
    }),
    "http://frigate:5000/api/events/1800000000.1-bm": () => ({
      id: "1800000000.1-bm",
      camera: "patio",
      label: "talos",
      start_time: 1800000000.1,
      end_time: ended ? 1800000020.1 : null,
      has_clip: ended,
      has_snapshot: false,
    }),
    "http://frigate:5000/api/events?": () => [
      { id: "1800000000.1-bm", camera: "patio", label: "talos", start_time: 1800000000.1 },
    ],
  });
  const c = new FrigateConnector({
    hubUrl: "http://hub",
    frigateUrl: "http://frigate:5000",
    propertyId: "house-01",
    cameras: CAMS,
    fetch: f,
    now: () => t,
  });
  expect(await c.commandsOnce()).toBe(1);
  expect(await c.commandsOnce()).toBe(0); // the same command is executed once
  const create = calls.find((x) => x.url.endsWith("/create"))!;
  expect(create.body).toMatchObject({ duration: 20, include_recording: true });
  const ack = calls.find((x) => x.url === "http://hub/v1/devices/acks")!;
  expect(ack.body).toMatchObject({ commandId: "cmd:rule1:e1", accepted: true });
  expect(await c.pollOnce()).toBe(0); // its own event is not a detection
  expect(await c.clipsOnce()).toBe(0); // still recording
  ended = true;
  t += 25_000;
  expect(await c.clipsOnce()).toBe(1);
  const clip = calls.filter((x) => x.url === "http://hub/v1/devices/observations").at(-1)!;
  expect(clip.body).toMatchObject({
    source: { deviceId: "cam_patio_video", capabilityId: "camera.recording" },
    eventType: "camera.clip_recorded",
    payload: {
      clipRecorded: { commandId: "cmd:rule1:e1", media: { mediaId: "1800000000.1-bm", hasClip: true } },
    },
  });
  expect(c.pendingClips).toBe(0);
});

test("Frigate failing a bookmark is ACKed as rejected; a clip that never comes is dropped", async () => {
  let t = 1_800_000_000_000;
  let createOk = false;
  const { calls, f } = harness({
    "http://hub/v1/devices/commands": () => ({
      commands: [
        { id: createOk ? "b2" : "b1", operation: "camera.bookmark", args: { bookmark: {} } },
        { id: "x1", operation: "audio.speak" },
      ],
    }),
    "http://frigate:5000/api/events/patio/talos/create": () =>
      createOk
        ? { success: true, event_id: "e-never" }
        : new Response("camera not found", { status: 404 }),
    "http://frigate:5000/api/events/e-never": () => new Response("busy", { status: 500 }),
  });
  const c = new FrigateConnector({
    hubUrl: "http://hub",
    frigateUrl: "http://frigate:5000",
    propertyId: "house-01",
    cameras: CAMS,
    fetch: f,
    now: () => t,
  });
  await c.commandsOnce();
  const acks = calls.filter((x) => x.url === "http://hub/v1/devices/acks").map((x) => x.body);
  expect(acks).toEqual([
    { commandId: "b1", accepted: false, detail: "frigate 404" },
    { commandId: "x1", accepted: false, detail: "unsupported command" },
  ]);
  createOk = true;
  await c.commandsOnce();
  expect(c.pendingClips).toBe(1);
  t += 200_000; // past the default 30 s bookmark plus the grace period
  expect(await c.clipsOnce()).toBe(0);
  expect(c.pendingClips).toBe(0);
});

test("one duty failing never skips the others", async () => {
  const { calls, f } = harness({
    "http://frigate:5000/api/events?": () => new Response("down", { status: 502 }),
    "http://hub/v1/devices/commands": () => ({ commands: [] }),
  });
  const c = new FrigateConnector({
    hubUrl: "http://hub",
    frigateUrl: "http://frigate:5000",
    propertyId: "house-01",
    cameras: CAMS,
    fetch: f,
  });
  const errors = await c.tick();
  expect(errors).toEqual(["Error: frigate 502"]);
  expect(calls.some((x) => x.url === "http://hub/v1/devices/commands")).toBe(true);
});
