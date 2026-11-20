import { describe, expect, test } from "vitest";
import { HubClient, HubError, hubUrl } from "./client.ts";
import { ArmMode } from "./gen/talos/v1/rule_pb.ts";

function fakeFetch(handler: (url: URL, init?: RequestInit) => Response): typeof fetch {
  return (async (input: string | URL | Request, init?: RequestInit) =>
    handler(new URL(input.toString()), init)) as typeof fetch;
}

describe("HubClient", () => {
  test("sends bearer token and decodes responses", async () => {
    let auth = "";
    const client = new HubClient({
      baseUrl: "http://hub.local",
      token: "t0k",
      fetch: fakeFetch((url, init) => {
        auth = new Headers(init?.headers).get("Authorization") ?? "";
        expect(url.pathname).toBe("/v1/state");
        return Response.json({
          propertyId: "house-01",
          throughSeq: "7",
          armMode: "ARM_MODE_ARMED_AWAY",
        });
      }),
    });
    const s = await client.state();
    expect(auth).toBe("Bearer t0k");
    expect(s.throughSeq).toBe(7n);
    expect(s.armMode).toBe(ArmMode.ARMED_AWAY);
  });

  test("maps error bodies to HubError", async () => {
    const client = new HubClient({
      baseUrl: "http://hub.local",
      token: "x",
      fetch: fakeFetch(() =>
        Response.json({ code: "denied", message: "requires_human_authorization" }, { status: 403 }),
      ),
    });
    await expect(client.setArmMode(ArmMode.DISARMED)).rejects.toMatchObject({
      status: 403,
      code: "denied",
    } satisfies Partial<HubError>);
  });

  test("parses the SSE journal stream", async () => {
    const frames =
      ': keepalive\n\nid: 1\nevent: journal\ndata: {"journalSeq":"1","receivedAtUnixMs":"5","armModeChanged":{"mode":"ARM_MODE_ARMED_AWAY","principalId":"owner"}}\n\n';
    const client = new HubClient({
      baseUrl: "http://hub.local",
      token: "x",
      fetch: fakeFetch(() => new Response(frames)),
    });
    const got = [];
    for await (const e of client.events(0n)) got.push(e);
    expect(got).toHaveLength(1);
    expect(got[0]?.journalSeq).toBe(1n);
    expect(got[0]?.event.case).toBe("armModeChanged");
  });

  test("a non-JSON error body still becomes a HubError", async () => {
    const client = new HubClient({
      baseUrl: "http://hub.local",
      token: "x",
      fetch: fakeFetch(
        () =>
          new Response("<html>502 Bad Gateway</html>", { status: 502, statusText: "Bad Gateway" }),
      ),
    });
    const err = await client.state().catch((e: unknown) => e);
    expect(err).toBeInstanceOf(HubError);
    expect(err).toMatchObject({ status: 502, code: "http_502" });
    const empty = new HubClient({
      baseUrl: "http://hub.local",
      token: "x",
      fetch: fakeFetch(() => new Response(null, { status: 404 })),
    });
    await expect(empty.state()).rejects.toMatchObject({ status: 404, code: "http_404" });
  });

  test("a non-JSON success body is a HubError, not a SyntaxError", async () => {
    const client = new HubClient({
      baseUrl: "http://hub.local",
      token: "x",
      fetch: fakeFetch(() => new Response("ok")),
    });
    await expect(client.state()).rejects.toMatchObject({ code: "bad_response" });
  });

  test("keeps a base-path prefix when building URLs", async () => {
    expect(hubUrl("https://x.example/talos", "/v1/state").href).toBe(
      "https://x.example/talos/v1/state",
    );
    expect(hubUrl("https://x.example/talos/", "/v1/journal?after=3&limit=9").href).toBe(
      "https://x.example/talos/v1/journal?after=3&limit=9",
    );
    expect(hubUrl("http://hub.local", "/v1/health").href).toBe("http://hub.local/v1/health");
    const seen: string[] = [];
    const client = new HubClient({
      baseUrl: "https://x.example/api/hub",
      token: "x",
      fetch: fakeFetch((url) => {
        seen.push(url.pathname);
        return Response.json({});
      }),
    });
    await client.state();
    expect(seen).toEqual(["/api/hub/v1/state"]);
  });

  test("an idle event stream fails with stream_idle", async () => {
    // Headers arrive, then nothing: no frames, no keepalives.
    const stalled = new ReadableStream<Uint8Array>({ start() {} });
    const client = new HubClient({
      baseUrl: "http://hub.local",
      token: "x",
      fetch: fakeFetch(() => new Response(stalled)),
    });
    const run = async () => {
      for await (const _ of client.events(0n, undefined, { idleTimeoutMs: 20 })) void _;
    };
    await expect(run()).rejects.toMatchObject({ code: "stream_idle" });
  });

  test("keepalives reset the idle timer", async () => {
    const enc = new TextEncoder();
    let n = 0;
    const body = new ReadableStream<Uint8Array>({
      async pull(c) {
        await new Promise((r) => setTimeout(r, 10));
        if (n++ < 5) c.enqueue(enc.encode(": keepalive\n\n"));
        else c.close();
      },
    });
    const client = new HubClient({
      baseUrl: "http://hub.local",
      token: "x",
      fetch: fakeFetch(() => new Response(body)),
    });
    const got = [];
    for await (const e of client.events(0n, undefined, { idleTimeoutMs: 30 })) got.push(e);
    expect(got).toHaveLength(0); // ended normally after ~60 ms, never idle
  });

  test("a hung connect also times out", async () => {
    const client = new HubClient({
      baseUrl: "http://hub.local",
      token: "x",
      fetch: ((_url: string | URL, init?: RequestInit) =>
        new Promise((_, reject) =>
          init?.signal?.addEventListener("abort", () => reject(new Error("aborted"))),
        )) as typeof fetch,
    });
    const run = async () => {
      for await (const _ of client.events(0n, undefined, { idleTimeoutMs: 20 })) void _;
    };
    await expect(run()).rejects.toMatchObject({ code: "stream_idle" });
  });
});
