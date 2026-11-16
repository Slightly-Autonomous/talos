// Browser E2E for the Phase 4 exit scenario. Boots the real hub binary (built
// by Bazel) on a fresh journal, the devsim virtual devices, and the web app.
import { defineConfig, devices } from "@playwright/test";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL("..", import.meta.url));
const hubBin = process.env.TALOS_HUB_BIN ?? join(root, "bazel-bin/server/talos_hub");
const journal = join(mkdtempSync(join(tmpdir(), "talos-e2e-")), "journal.db");

export default defineConfig({
  testDir: "e2e",
  timeout: 60_000,
  expect: { timeout: 15_000 },
  fullyParallel: false,
  workers: 1,
  reporter: process.env.CI ? [["list"], ["html", { open: "never" }]] : "list",
  use: {
    baseURL: "http://localhost:5173",
    trace: "retain-on-failure",
    screenshot: "only-on-failure",
  },
  projects: [{ name: "chromium", use: { ...devices["Desktop Chrome"] } }],
  webServer: [
    {
      command: `${hubBin} --config ${join(root, "deploy/config/hub.dev.yaml")}`,
      url: "http://127.0.0.1:8080/v1/health",
      env: { TALOS_SQLITE_PATH: journal, PORT: "8080" },
      reuseExistingServer: false,
      stdout: "pipe",
    },
    {
      command: "pnpm --filter @talos/devsim start",
      cwd: root,
      url: "http://127.0.0.1:8090/v1/sim/status",
      env: { HUB_URL: "http://127.0.0.1:8080" },
      reuseExistingServer: false,
    },
    {
      command: "pnpm dev --port 5173 --strictPort",
      url: "http://localhost:5173",
      env: { VITE_HUB_URL: "http://127.0.0.1:8080", VITE_DEVSIM_URL: "http://127.0.0.1:8090" },
      reuseExistingServer: false,
    },
  ],
});
