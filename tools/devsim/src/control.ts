// devsim's localhost control API (web Simulation panel and browser E2E).
// Development only, but still guarded against CSRF: any website a developer
// visits could otherwise POST motion or contact events into the dev hub.
import type { IncomingMessage, ServerResponse } from "node:http";
import { SNAPSHOT_JPEG, type DevSim, type Fault } from "./devsim.ts";

export interface ControlRequestMeta {
  method: string | undefined;
  origin: string | undefined;
  contentType: string | undefined;
}

// Returns why a request must be refused, or undefined to accept it.
// - A present Origin must be allow-listed (this also covers "null").
// - POST must be `application/json`, so a cross-site <form> or a text/plain
//   "simple" fetch can never act; a JSON fetch from another origin needs a
//   CORS preflight, which only allow-listed origins pass.
// Non-browser clients (curl, scripts) send no Origin and are accepted:
// browsers always attach one to cross-origin and POST requests.
export function refuseControlRequest(
  req: ControlRequestMeta,
  allowedOrigins: readonly string[],
): string | undefined {
  if (req.origin !== undefined && !allowedOrigins.includes(req.origin)) return "origin not allowed";
  if (req.method === "POST") {
    const type = (req.contentType ?? "").split(";")[0]!.trim().toLowerCase();
    if (type !== "application/json") return "content-type must be application/json";
  }
  return undefined;
}

export function controlHandler(sim: DevSim, allowedOrigins: readonly string[]) {
  return async (req: IncomingMessage, res: ServerResponse) => {
    const origin = req.headers.origin;
    const send = (status: number, body: unknown) =>
      res.writeHead(status, { "Content-Type": "application/json" }).end(JSON.stringify(body));
    if (origin && allowedOrigins.includes(origin)) {
      res.setHeader("Access-Control-Allow-Origin", origin);
      res.setHeader("Access-Control-Allow-Headers", "Content-Type");
      res.setHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
      res.setHeader("Vary", "Origin");
    }
    if (req.method === "OPTIONS") return res.writeHead(204).end();
    const refused = refuseControlRequest(
      { method: req.method, origin, contentType: req.headers["content-type"] },
      allowedOrigins,
    );
    if (refused) return send(403, { error: refused });
    try {
      let body: Record<string, unknown> = {};
      if (req.method === "POST") {
        const chunks: Buffer[] = [];
        for await (const c of req) chunks.push(c as Buffer);
        body = JSON.parse(Buffer.concat(chunks).toString() || "{}") as Record<string, unknown>;
      }
      const id = String(body.device_id ?? "");
      // The fake recorder's media (ADR 0020): what the hub's media proxy fetches when the
      // hub runs with TALOS_FRIGATE_URL pointing here. Only ids devsim reported exist.
      const media = /^\/api\/events\/([A-Za-z0-9._-]+)\/(snapshot\.jpg|clip\.mp4)$/.exec(
        req.url ?? "",
      );
      if (req.method === "GET" && media) {
        if (!sim.media.has(media[1]!) || media[2] !== "snapshot.jpg") return send(404, {});
        return res.writeHead(200, { "Content-Type": "image/jpeg" }).end(SNAPSHOT_JPEG);
      }
      switch (`${req.method} ${req.url}`) {
        case "GET /v1/sim/status":
          return send(200, sim.status());
        case "POST /v1/sim/motion":
          await sim.motion(id);
          return send(200, { ok: true });
        case "POST /v1/sim/contact":
          await sim.contact(id, body.open === true);
          return send(200, { ok: true });
        case "POST /v1/sim/tamper":
          await sim.tamper(id, body.open === true);
          return send(200, { ok: true });
        case "POST /v1/sim/user_button":
          await sim.userButton(id);
          return send(200, { ok: true });
        case "POST /v1/sim/stream":
          await sim.stream(id, body.up === true);
          return send(200, { ok: true });
        case "POST /v1/sim/detect":
          return send(200, { mediaId: await sim.detect(id, String(body.label ?? "person")) });
        case "POST /v1/sim/fault":
          sim.setFault(id, String(body.fault) as Fault);
          return send(200, { ok: true });
        default:
          return send(404, { error: "not found" });
      }
    } catch (e) {
      return send(400, { error: String(e) });
    }
  };
}
