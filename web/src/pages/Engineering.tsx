// Engineering view: the raw truth behind the homeowner screens. Disclosure
// depth is a UI choice; it grants no extra authority.
import { useState } from "react";
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
  const { state, entries } = useHub();
  const [open, setOpen] = useState<string>();
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
