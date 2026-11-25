// ADR 0020: a Vision Node in the browser. Its stream status, detections with snapshots fetched
// through the hub's media proxy (devsim plays Frigate), stream loss shown as DOWN, and a bookmark
// verified only by its clip.
import { expect, test } from "@playwright/test";

const DEVSIM = "http://127.0.0.1:8090";

test("camera stream, snapshots through the hub, stream loss and a verified bookmark", async ({
  page,
  request,
}) => {
  await page.goto("/");
  await page.getByTestId("hub-token").fill("dev-owner-token");
  await page.getByTestId("connect").click();
  await expect(page.getByTestId("connection")).toContainText("Live");

  const camera = page.getByTestId("camera-cam_patio");
  await expect(camera.getByTestId("stream-cam_patio")).toHaveAttribute("data-stream", "streaming");

  // Frigate (devsim) detects a person: the card shows it with the snapshot the hub served.
  const r = await request.post(`${DEVSIM}/v1/sim/detect`, {
    data: { device_id: "cam_patio_video", label: "person" },
  });
  expect(r.ok()).toBeTruthy();
  const img = camera.getByRole("img", { name: /person at/ });
  await expect(img).toBeVisible();
  await expect
    .poll(() => img.evaluate((el) => (el as HTMLImageElement).naturalWidth))
    .toBeGreaterThan(0);

  // Motion in the backyard while armed: the dev rule bookmarks the footage (verified by the clip).
  await page.getByTestId("arm-away").click();
  await expect(page.getByTestId("zone-backyard")).not.toHaveAttribute("data-activity", "active", {
    timeout: 20_000,
  });
  await page.getByRole("tab", { name: "Simulation" }).click();
  await page.getByTestId("sim-backyard-motion").click();
  await page.getByRole("tab", { name: "Live" }).click();
  await expect(
    page.getByTestId("command-camera.bookmark").first().getByTestId("outcome"),
  ).toHaveText("Verified", { timeout: 20_000 });

  // The camera stops producing video: shown as DOWN, never as streaming.
  await request.post(`${DEVSIM}/v1/sim/stream`, { data: { device_id: "cam_patio", up: false } });
  await expect(page.getByTestId("stream-cam_patio")).toHaveAttribute("data-stream", "down");
  await request.post(`${DEVSIM}/v1/sim/stream`, { data: { device_id: "cam_patio", up: true } });
  await expect(page.getByTestId("stream-cam_patio")).toHaveAttribute("data-stream", "streaming");
});
