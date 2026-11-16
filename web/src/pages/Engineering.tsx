// Engineering view: the raw truth behind the homeowner screens. Disclosure
// depth is a UI choice; it grants no extra authority.
import { useEffect, useState } from "react";
import type { Diagnostic } from "@talos/contracts/talos/v1/health_pb";
import { toJson } from "@bufbuild/protobuf";
import { Freshness } from "@talos/contracts/talos/api/v1/api_pb";
import { JournalEntrySchema } from "@talos/contracts/talos/v1/journal_pb";
import { useHub } from "../hub/HubProvider";
import { capabilityLabel, describeEntry } from "../model/view";

const FRESH = {
  [Freshness.FRESH]: "fresh",
  [Freshness.STALE]: "stale",
  [Freshness.UNKNOWN]: "unknown",
} as Record<number, string>;
const ORDER = ["?", "in order", "gap", "late", "new boot"];

export function Engineering() {
  const { client, property, state, entries } = useHub();
  const [open, setOpen] = useState<string>();
  const [device, setDevice] = useState<string>("");
  const [diag, setDiag] = useState<Diagnostic[]>([]);
  useEffect(() => {
    if (!device) return;
    let stop = false;
    const load = () =>
      client
        .diagnostics(device)
        .then((r) => !stop && setDiag(r.diagnostics))
        .catch(() => setDiag([]));
    void load();
    const t = setInterval(load, 3000);
    return () => {
      stop = true;
      clearInterval(t);
    };
  }, [client, device]);
  return (
    <div className="engineering">
      <section className="panel">
        <h3>Capabilities (through journal seq {String(state?.throughSeq ?? 0n)})</h3>
        <table>
          <thead>
            <tr>
              <th>Device</th>
              <th>Slot</th>
              <th>Capability</th>
              <th>Value</th>
              <th>Freshness</th>
              <th>Received</th>
              <th>Seq</th>
            </tr>
          </thead>
          <tbody>
            {state?.capabilities.map((c) => (
              <tr key={`${c.ref?.deviceId}/${c.ref?.slot}/${c.ref?.capabilityId}`}>
                <td>{c.ref?.deviceId}</td>
                <td>{c.ref?.slot ?? "—"}</td>
                <td>{c.ref?.capabilityId}</td>
                <td>{capabilityLabel(c)}</td>
                <td className={`fresh-${FRESH[c.freshness]}`}>{FRESH[c.freshness]}</td>
                <td>{new Date(Number(c.receivedAtUnixMs)).toLocaleTimeString()}</td>
                <td>{String(c.journalSeq)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </section>
      <section className="panel">
        <h3>Device diagnostics</h3>
        <p className="muted small">
          Observability plane (ADR 0014): bounded, in-memory, never the journal.
        </p>
        <select value={device} onChange={(e) => setDevice(e.target.value)} aria-label="Device">
          <option value="">choose a device…</option>
          {property?.devices.map((d) => (
            <option key={d.id}>{d.id}</option>
          ))}
        </select>
        {device && diag.length === 0 && (
          <p className="muted">No diagnostics (HTTP devices report none).</p>
        )}
        {diag.length > 0 && (
          <table>
            <thead>
              <tr>
                <th>Time</th>
                <th>Kind</th>
                <th>Value</th>
                <th>Correlation</th>
              </tr>
            </thead>
            <tbody>
              {[...diag].reverse().map((d) => (
                <tr key={d.correlationId}>
                  <td>{new Date(Number(d.atUnixMs)).toLocaleTimeString()}</td>
                  <td>{d.kind}</td>
                  <td>
                    {String(d.value)} {d.unit}
                  </td>
                  <td className="small">{d.correlationId}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </section>
      <section className="panel">
        <h3>Journal ({entries.length} entries)</h3>
        <table data-testid="journal-table">
          <thead>
            <tr>
              <th>Seq</th>
              <th>Received</th>
              <th>Entry</th>
              <th>Source order</th>
            </tr>
          </thead>
          <tbody>
            {[...entries]
              .reverse()
              .slice(0, 200)
              .map((e) => {
                const key = String(e.journalSeq);
                return (
                  <tr
                    key={key}
                    onClick={() => setOpen(open === key ? undefined : key)}
                    className="clickable"
                  >
                    <td>{key}</td>
                    <td>
                      {new Date(Number(e.receivedAtUnixMs)).toLocaleTimeString()}
                      {e.clockRegressed ? " ⚠ clock" : ""}
                    </td>
                    <td>
                      {describeEntry(e)}
                      {open === key && (
                        <pre>{JSON.stringify(toJson(JournalEntrySchema, e), null, 2)}</pre>
                      )}
                    </td>
                    <td>{e.event.case === "observation" ? ORDER[e.event.value.order] : ""}</td>
                  </tr>
                );
              })}
          </tbody>
        </table>
      </section>
    </div>
  );
}
