import { describe, expect, test } from "vitest";
import { HubClient, HubError } from "./client.ts";
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
});
