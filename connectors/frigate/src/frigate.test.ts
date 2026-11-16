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
