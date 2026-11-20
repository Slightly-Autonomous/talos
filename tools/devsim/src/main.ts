// devsim: virtual devices + a localhost control API for the web Simulation
// panel and browser E2E. Development only (dev tokens).
//   HUB_URL (default http://localhost:8080), DEVSIM_PORT (8090),
//   DEVSIM_ORIGINS (comma list, default the Vite dev/preview origins). POSTs
//   need Content-Type application/json and, from a browser, one of these.
import { createServer } from "node:http";
import { controlHandler } from "./control.ts";
import { DEV_DEVICES, DevSim } from "./devsim.ts";

const hubUrl = process.env.HUB_URL ?? "http://localhost:8080";
const port = Number(process.env.DEVSIM_PORT ?? 8090);
const origins = (
  process.env.DEVSIM_ORIGINS ??
  "http://localhost:5173,http://localhost:4173,http://127.0.0.1:5173,http://127.0.0.1:4173"
).split(",");

const sim = new DevSim({ hubUrl, devices: DEV_DEVICES, doorContactDevice: "node-12" });

async function bootWithRetry() {
  for (let attempt = 1; ; attempt++) {
    try {
      await sim.boot();
      return;
    } catch (e) {
      if (attempt % 10 === 1) console.error(`waiting for hub at ${hubUrl}: ${String(e)}`);
      await new Promise((r) => setTimeout(r, 500));
    }
  }
}

await bootWithRetry();
sim.start();

const server = createServer(controlHandler(sim, origins));

server.listen(port, "127.0.0.1", () =>
  console.log(`devsim on http://127.0.0.1:${port} -> hub ${hubUrl}`),
);
