import { createServer, type Server } from "node:http";
import type { AddressInfo } from "node:net";
import { afterEach, describe, expect, test } from "vitest";
import { controlHandler, refuseControlRequest } from "./control.ts";
import type { DevSim } from "./devsim.ts";

const ALLOWED = ["http://localhost:5173"];

describe("refuseControlRequest", () => {
  test("accepts JSON POSTs from an allow-listed origin or from no-origin tools", () => {
    const ok = {
      method: "POST",
      contentType: "application/json; charset=utf-8",
    };
    expect(
      refuseControlRequest({ ...ok, origin: "http://localhost:5173" }, ALLOWED),
    ).toBeUndefined();
    expect(refuseControlRequest({ ...ok, origin: undefined }, ALLOWED)).toBeUndefined();
  });
  test("refuses other origins, including the opaque null origin", () => {
    const req = { method: "POST", contentType: "application/json" };
    expect(refuseControlRequest({ ...req, origin: "https://evil.example" }, ALLOWED)).toMatch(
      /origin/,
    );
    expect(refuseControlRequest({ ...req, origin: "null" }, ALLOWED)).toMatch(/origin/);
    expect(
      refuseControlRequest(
        {
          method: "GET",
          contentType: undefined,
          origin: "https://evil.example",
        },
        ALLOWED,
      ),
    ).toMatch(/origin/);
  });
  test("refuses POSTs that are not application/json (form and text/plain CSRF)", () => {
    for (const contentType of [undefined, "text/plain", "application/x-www-form-urlencoded"])
      expect(
        refuseControlRequest(
          { method: "POST", origin: "http://localhost:5173", contentType },
          ALLOWED,
        ),
      ).toMatch(/content-type/);
  });
});

describe("controlHandler", () => {
  let server: Server | undefined;
  afterEach(() => server?.close());

  async function start() {
    const motions: string[] = [];
    const sim = {
      motion: async (id: string) => void motions.push(id),
      status: () => ({ devices: {} }),
    } as unknown as DevSim;
    server = createServer(controlHandler(sim, ALLOWED));
    await new Promise<void>((r) => server!.listen(0, "127.0.0.1", r));
    const url = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
    return { motions, url };
  }

  test("a text/plain cross-site POST injects nothing", async () => {
    const { motions, url } = await start();
    const res = await fetch(`${url}/v1/sim/motion`, {
      method: "POST",
      headers: { "Content-Type": "text/plain", Origin: "https://evil.example" },
      body: JSON.stringify({ device_id: "node-07" }),
    });
    expect(res.status).toBe(403);
    const plain = await fetch(`${url}/v1/sim/motion`, {
      method: "POST",
      headers: {
        "Content-Type": "text/plain",
        Origin: "http://localhost:5173",
      },
      body: JSON.stringify({ device_id: "node-07" }),
    });
    expect(plain.status).toBe(403);
    expect(motions).toEqual([]);
  });

  test("the web panel's JSON POST works and gets CORS headers", async () => {
    const { motions, url } = await start();
    const res = await fetch(`${url}/v1/sim/motion`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Origin: "http://localhost:5173",
      },
      body: JSON.stringify({ device_id: "node-07" }),
    });
    expect(res.status).toBe(200);
    expect(res.headers.get("access-control-allow-origin")).toBe("http://localhost:5173");
    expect(motions).toEqual(["node-07"]);
  });
});
