// Spec Phase 4 exit: open the virtual property, trigger backyard motion,
// watch the speaker command verify, and replay the complete history.
import { expect, test } from "@playwright/test";

test("backyard intrusion end to end, then replay", async ({ page }) => {
  await page.goto("/");
  await page.getByTestId("hub-token").fill("dev-owner-token");
  await page.getByTestId("connect").click();
  await expect(page.getByTestId("connection")).toContainText("Live");

  // Arm away.
  await page.getByTestId("arm-away").click();
  await expect(page.getByTestId("arm-mode")).toHaveText("Armed away");

  // Trigger the virtual PIR through devsim (a real device-API client).
  await page.getByRole("tab", { name: "Simulation" }).click();
  await page.getByTestId("sim-backyard-motion").click();

  // Live view: backyard shaded active, speaker warning verified.
  await page.getByRole("tab", { name: "Live" }).click();
  await expect(page.getByTestId("zone-backyard")).toHaveAttribute("data-activity", "active");
  const speak = page.getByTestId("command-audio.speak").first();
  await expect(speak.getByTestId("outcome")).toHaveText("Verified");
  await expect(page.getByTestId("portal-front_door")).toHaveAttribute("data-open", "false");

  // Replay: the start of history is disarmed with no commands; the end shows
  // the verified warning and the motion event.
  await page.getByRole("tab", { name: "Replay" }).click();
  const slider = page.getByTestId("replay-slider");
  await expect(slider).toBeVisible();
  const min = await slider.getAttribute("min");
  await slider.fill(min!);
  await expect(page.getByTestId("replay-arm")).toHaveText("Disarmed");
  const max = await slider.getAttribute("max");
  await slider.fill(max!);
  await expect(page.getByTestId("replay-arm")).toHaveText("Armed away");
  await expect(page.getByTestId("replay-events")).toContainText("motion.detected from node-07");
  await expect(page.getByTestId("command-audio.speak").first().getByTestId("outcome")).toHaveText(
    "Verified",
  );
});

test("a speaker that only ACKs is never shown as verified", async ({ page }) => {
  await page.goto("/");
  await page.getByTestId("hub-token").fill("dev-owner-token");
  await page.getByTestId("connect").click();
  await page.getByTestId("arm-away").click();
  // Tests share one hub and devsim: wait for any earlier motion episode to
  // clear so the next trigger is a new detection (and a new command).
  await expect(page.getByTestId("zone-backyard")).not.toHaveAttribute("data-activity", "active", {
    timeout: 20_000,
  });
  await page.getByRole("tab", { name: "Simulation" }).click();
  await page.getByRole("button", { name: "patio_speaker: silent_ack" }).click();
  await page.getByTestId("sim-backyard-motion").click();
  await page.getByRole("tab", { name: "Live" }).click();
  // The newest speak command ends unverified once the 3 s window closes.
  await expect(page.getByTestId("command-audio.speak").first().getByTestId("outcome")).toHaveText(
    "Unverified",
    { timeout: 20_000 },
  );
  await page.getByRole("tab", { name: "Simulation" }).click();
  await page.getByRole("button", { name: "patio_speaker: none" }).click();
});

test("a wrong token is refused", async ({ page }) => {
  await page.goto("/");
  await page.getByTestId("hub-token").fill("not-a-token");
  await page.getByTestId("connect").click();
  await expect(page.getByRole("alert")).toContainText("401");
});
