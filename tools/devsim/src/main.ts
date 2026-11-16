// devsim: virtual devices + a localhost control API for the web Simulation
// panel and browser E2E. Development only (dev tokens).
//   HUB_URL (default http://localhost:8080), DEVSIM_PORT (8090),
//   DEVSIM_ORIGINS (comma list, default the Vite dev/preview origins)
import { createServer } from "node:http";
import { DEV_DEVICES, DevSim, type Fault } from "./devsim.ts";

const hubUrl = process.env.HUB_URL ?? "http://localhost:8080";
const port = Number(process.env.DEVSIM_PORT ?? 8090);
const origins = (process.env.DEVSIM_ORIGINS ?? "http://localhost:5173,http://localhost:4173").split(
  ",",
);

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

const server = createServer(async (req, res) => {
  const origin = req.headers.origin;
  if (origin && origins.includes(origin)) {
    res.setHeader("Access-Control-Allow-Origin", origin);
    res.setHeader("Access-Control-Allow-Headers", "Content-Type");
    res.setHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  }
  if (req.method === "OPTIONS") return res.writeHead(204).end();
  const send = (status: number, body: unknown) =>
    res.writeHead(status, { "Content-Type": "application/json" }).end(JSON.stringify(body));
  try {
    let body: Record<string, unknown> = {};
    if (req.method === "POST") {
      const chunks: Buffer[] = [];
      for await (const c of req) chunks.push(c as Buffer);
      body = JSON.parse(Buffer.concat(chunks).toString() || "{}");
    }
    const id = String(body.device_id ?? "");
    switch (`${req.method} ${req.url}`) {
      case "GET /v1/sim/status":
        return send(200, sim.status());
      case "POST /v1/sim/motion":
        await sim.motion(id);
        return send(200, { ok: true });
      case "POST /v1/sim/contact":
        await sim.contact(id, body.open === true);
        return send(200, { ok: true });
      case "POST /v1/sim/fault":
        sim.setFault(id, String(body.fault) as Fault);
        return send(200, { ok: true });
      default:
        return send(404, { error: "not found" });
    }
  } catch (e) {
    return send(400, { error: String(e) });
  }
});

server.listen(port, "127.0.0.1", () =>
  console.log(`devsim on http://127.0.0.1:${port} -> hub ${hubUrl}`),
);
