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

// Phase 4b (ADR 0012): edits are versioned; history keeps the old layout and
// new rules are previewed against history, then take effect.
test("layout and rule edits are versioned and take effect", async ({ page }) => {
  await page.goto("/");
  await page.getByTestId("hub-token").fill("dev-owner-token");
  await page.getByTestId("connect").click();
  await expect(page.getByTestId("connection")).toContainText("Live");
  await page.getByTestId("arm-away").click();
  await expect(page.getByTestId("arm-mode")).toHaveText("Armed away");

  // History for the preview: the front door opens while armed away.
  await page.getByRole("tab", { name: "Simulation" }).click();
  await page.getByRole("button", { name: "Open front door" }).click();
  await page.getByRole("button", { name: "Close front door" }).click();

  // Layout: rename the patio.
  await page.getByRole("tab", { name: "Configure" }).click();
  await page.getByTestId("layout-zone").selectOption("patio");
  await page.getByTestId("layout-zone-name").fill("Back patio");
  await expect(page.getByTestId("layout-diff")).toContainText(
    'rename patio: "Patio" → "Back patio"',
  );
  await page.getByTestId("save-layout").click();
  await expect(page.getByTestId("save-status")).toContainText("Saved as revision");

  // Rules: a front-door chime, previewed against history, then saved.
  await page.getByRole("button", { name: "Rules" }).click();
  await page.getByTestId("rule-select").selectOption("__new");
  await page.getByTestId("rule-id").fill("front_door_chime");
  await page.getByTestId("rule-trigger").selectOption("contact.opened");
  await page.getByTestId("rule-zone").selectOption("hallway");
  await page.getByTestId("rule-target").selectOption("patio_speaker");
  await page.getByTestId("rule-text").fill("Front door opened");
  await page.getByTestId("rule-preview").click();
  await expect(page.getByTestId("preview-summary")).toContainText(/Would have fired [1-9]/);
  await page.getByTestId("rule-save").click();
  await expect(page.getByTestId("save-status")).toContainText("Saved as revision");

  // The live plan shows the new name; the new rule fires for real.
  await page.getByRole("tab", { name: "Live" }).click();
  await expect(page.getByTestId("zone-patio")).toContainText("Back patio");
  await page.getByRole("tab", { name: "Simulation" }).click();
  await page.getByRole("button", { name: "Open front door" }).click();
  await page.getByRole("tab", { name: "Live" }).click();
  await expect(page.getByTestId("command-audio.speak").first().getByTestId("outcome")).toHaveText(
    "Verified",
  );

  // Replay at the start of history draws the layout in force then.
  await page.getByRole("tab", { name: "Replay" }).click();
  const slider = page.getByTestId("replay-slider");
  await slider.fill((await slider.getAttribute("min"))!);
  await expect(page.getByTestId("zone-patio")).toContainText("Patio");
  await expect(page.getByTestId("zone-patio")).not.toContainText("Back patio");
});

// HD-032: an open enclosure is an alarm in every mode and shows as a fault, never healthy.
test("enclosure tamper alarms even disarmed and shows as a fault until closed", async ({
  page,
}) => {
  await page.goto("/");
  await page.getByTestId("hub-token").fill("dev-owner-token");
  await page.getByTestId("connect").click();
  await expect(page.getByTestId("connection")).toContainText("Live");
  await page.getByTestId("disarm").click();
  await expect(page.getByTestId("arm-mode")).toHaveText("Disarmed");
  // devsim reported the lid closed at boot.
  await expect(page.getByTestId("tamper-node-12")).toHaveAttribute("data-tamper", "closed");

  await page.getByRole("tab", { name: "Simulation" }).click();
  await page.getByTestId("sim-node-12-lid-open").click();
  await page.getByRole("tab", { name: "Live" }).click();
  const tamper = page.getByTestId("tamper-node-12");
  await expect(tamper).toHaveAttribute("data-tamper", "open");
  await expect(tamper).toContainText("TAMPER");
  await expect(page.getByTestId("health-node-12")).not.toContainText("healthy");
  await expect(page.getByTestId("device-node-12").locator("circle")).toHaveClass(/device-fault/);
  // The 24-hour tamper rule fired while disarmed (dev config: the patio speaker).
  await expect(page.getByTestId("recent-events")).toContainText("tamper.opened from node-12");
  await expect(page.getByTestId("command-audio.speak").first().getByTestId("outcome")).toHaveText(
    "Verified",
  );

  await page.getByRole("tab", { name: "Simulation" }).click();
  await page.getByTestId("sim-node-12-lid-close").click();
  await page.getByRole("tab", { name: "Live" }).click();
  await expect(page.getByTestId("tamper-node-12")).toHaveAttribute("data-tamper", "closed");
  await expect(page.getByTestId("recent-events")).toContainText("tamper.closed from node-12");
});

// Owner decision 2026-11-21: maintenance suppresses one node's tamper alarm for a bounded time;
// cancelling it with the lid still open raises the alarm then.
test("maintenance suppresses the tamper alarm, and ending it with the lid open alarms", async ({
  page,
}) => {
  await page.goto("/");
  await page.getByTestId("hub-token").fill("dev-owner-token");
  await page.getByTestId("connect").click();
  await expect(page.getByTestId("connection")).toContainText("Live");
  await expect(page.getByTestId("tamper-node-12")).toHaveAttribute("data-tamper", "closed");

  await page.getByTestId("maint-start-node-12").click();
  await expect(page.getByTestId("maint-node-12")).toContainText("30 min");
  await expect(page.getByTestId("health-node-12")).toContainText("maintenance");
  await expect(page.getByTestId("health-node-12")).not.toContainText("healthy");
  const speaks = page.getByTestId("command-audio.speak");
  const before = await speaks.count();

  await page.getByRole("tab", { name: "Simulation" }).click();
  await page.getByTestId("sim-node-12-lid-open").click();
  await page.getByRole("tab", { name: "Live" }).click();
  await expect(page.getByTestId("tamper-node-12")).toHaveAttribute("data-tamper", "open");
  await expect(page.getByTestId("recent-events")).toContainText(
    "tamper alarm for node-12 suppressed",
  );
  await expect(speaks).toHaveCount(before);

  await page.getByTestId("maint-end-node-12").click();
  await expect(page.getByTestId("maint-start-node-12")).toBeVisible();
  await expect(speaks).toHaveCount(before + 1);
  await expect(speaks.first().getByTestId("outcome")).toHaveText("Verified");

  await page.getByRole("tab", { name: "Simulation" }).click();
  await page.getByTestId("sim-node-12-lid-close").click();
  await page.getByRole("tab", { name: "Live" }).click();
  await expect(page.getByTestId("tamper-node-12")).toHaveAttribute("data-tamper", "closed");
});
