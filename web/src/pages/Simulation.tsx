// Developer simulation panel. Talks to tools/devsim, a separate process that
// acts as real devices over the hub's device API, so everything here flows
// through the same ingress, rules and policy as physical hardware.
import { useEffect, useState } from "react";
import { HubError } from "@talos/contracts/client";
import { useHub } from "../hub/HubProvider";
import { requestLock } from "../model/commands";

const DEVSIM_URL = import.meta.env.VITE_DEVSIM_URL ?? "http://localhost:8090";

interface SimStatus {
  devices: Record<string, { fault: string; state: Record<string, unknown> }>;
}

async function sim(path: string, body?: unknown): Promise<unknown> {
  const res = await fetch(`${DEVSIM_URL}${path}`, {
    method: body === undefined ? "GET" : "POST",
    headers: { "Content-Type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  if (!res.ok) throw new Error(`${res.status} ${await res.text()}`);
  return res.json();
}

export function Simulation() {
  const { client } = useHub();
  const [status, setStatus] = useState<SimStatus>();
  const [message, setMessage] = useState<string>();

  const refresh = () =>
    sim("/v1/sim/status")
      .then((s) => setStatus(s as SimStatus))
      .catch(() => setStatus(undefined));
  useEffect(() => {
    void refresh();
    const t = setInterval(() => void refresh(), 2000);
    return () => clearInterval(t);
  }, []);

  const act = (label: string, fn: () => Promise<unknown>) => async () => {
    setMessage(undefined);
    try {
      await fn();
      setMessage(`${label}: sent`);
      void refresh();
    } catch (e) {
      setMessage(`${label} failed: ${e instanceof HubError ? e.message : String(e)}`);
    }
  };

  const lockDoor = (locked: boolean) => requestLock(client, "lock-3", locked);

  if (!status) {
    return (
      <section className="panel">
        <h2>Simulation</h2>
        <p className="muted">
          No device simulator at {DEVSIM_URL}. Start it with{" "}
          <code>pnpm --filter @talos/devsim start</code>.
        </p>
      </section>
    );
  }

  return (
    <section className="panel simulation">
      <h2>Simulation</h2>
      <p className="muted">
        Virtual devices from <code>tools/devsim</code> report over the hub&apos;s device API, the
        same path physical nodes use.
      </p>
      <div className="sim-actions">
        <button
          data-testid="sim-backyard-motion"
          onClick={act("Backyard motion", () => sim("/v1/sim/motion", { device_id: "node-07" }))}
        >
          Person crosses backyard
        </button>
        <button
          onClick={act("Open front door", () =>
            sim("/v1/sim/contact", { device_id: "node-12", open: true }),
          )}
        >
          Open front door
        </button>
        <button
          onClick={act("Close front door", () =>
            sim("/v1/sim/contact", { device_id: "node-12", open: false }),
          )}
        >
          Close front door
        </button>
        <button onClick={act("Lock front door", () => lockDoor(true))}>Lock front door</button>
        <button onClick={act("Unlock front door", () => lockDoor(false))}>Unlock front door</button>
      </div>
      <h3>Faults</h3>
      <div className="sim-actions">
        {["patio_speaker", "lock-3"].map((d) =>
          ["none", "silent_ack", "no_response", "jam"].map((f) => (
            <button
              key={`${d}-${f}`}
              className={status.devices[d]?.fault === f ? "selected" : ""}
              onClick={act(`${d} fault ${f}`, () =>
                sim("/v1/sim/fault", { device_id: d, fault: f }),
              )}
            >
              {d}: {f}
            </button>
          )),
        )}
      </div>
      {message && <p className="muted">{message}</p>}
      <h3>Virtual devices</h3>
      <pre className="small">{JSON.stringify(status.devices, null, 2)}</pre>
    </section>
  );
}
