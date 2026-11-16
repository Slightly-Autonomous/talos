// History & replay: scrub the hub's journal. State at each instant comes from
// the hub (/v1/state_at), which reduces the same journal the live engine uses.
import { useEffect, useMemo, useState } from "react";
import type { StateResponse } from "@talos/contracts/talos/api/v1/api_pb";
import { FloorPlan } from "../components/FloorPlan";
import { CommandList } from "../components/CommandList";
import { useHub } from "../hub/HubProvider";
import { armLabel, describeEntry, motionHeat } from "../model/view";

const SPEEDS = [1, 4, 16, 64];

export function Replay() {
  const { client, property, entries } = useHub();
  const t0 = entries.length ? Number(entries[0]!.receivedAtUnixMs) : 0;
  const t1 = entries.length ? Number(entries[entries.length - 1]!.receivedAtUnixMs) : 0;
  const [t, setT] = useState<number | undefined>(undefined);
  const [playing, setPlaying] = useState(false);
  const [speed, setSpeed] = useState(4);
  const [view, setView] = useState<StateResponse>();
  const at = t ?? t1;

  // Fetch the state at the scrub position (debounced).
  useEffect(() => {
    if (!entries.length) return;
    const h = setTimeout(() => {
      client
        .stateAt(at)
        .then(setView)
        .catch(() => {});
    }, 80);
    return () => clearTimeout(h);
  }, [client, at, entries.length]);

  useEffect(() => {
    if (!playing) return;
    const step = 100;
    const id = setInterval(() => {
      setT((prev) => {
        const next = (prev ?? t0) + step * speed;
        if (next >= t1) {
          setPlaying(false);
          return t1;
        }
        return next;
      });
    }, step);
    return () => clearInterval(id);
  }, [playing, speed, t0, t1]);

  const heat = useMemo(
    () => (property ? motionHeat(property, entries, t0, at) : new Map<string, number>()),
    [property, entries, t0, at],
  );
  const visible = useMemo(
    () =>
      entries
        .filter((e) => Number(e.receivedAtUnixMs) <= at)
        .reverse()
        .slice(0, 40),
    [entries, at],
  );

  if (!property) return <p className="muted">Loading…</p>;
  if (!entries.length) return <p className="muted">The journal is empty: nothing to replay yet.</p>;

  return (
    <div className="live">
      <section className="panel plan-panel">
        <div className="panel-head">
          <h2>Replay</h2>
          <span className="arm-mode" data-testid="replay-arm">
            {armLabel(view?.armMode)}
          </span>
        </div>
        <div className="scrubber">
          <button
            onClick={() => {
              if (playing) {
                setPlaying(false);
              } else {
                setT(at >= t1 ? t0 : at);
                setPlaying(true);
              }
            }}
          >
            {playing ? "Pause" : "Play"}
          </button>
          <select
            value={speed}
            onChange={(e) => setSpeed(Number(e.target.value))}
            aria-label="Speed"
          >
            {SPEEDS.map((s) => (
              <option key={s} value={s}>
                {s}×
              </option>
            ))}
          </select>
          <input
            type="range"
            min={t0}
            max={t1}
            // "any": a fixed step would snap the end below the last entry.
            step="any"
            value={at}
            onChange={(e) => {
              setPlaying(false);
              setT(Number(e.target.value));
            }}
            aria-label="Replay time"
            data-testid="replay-slider"
          />
          <time data-testid="replay-time">{new Date(at).toLocaleString()}</time>
        </div>
        <FloorPlan property={property} state={view} nowMs={at} heat={heat} />
        <p className="legend muted">
          Shading = motion detections per zone from the start of history to the scrub time.
        </p>
      </section>
      <aside className="side">
        <section className="panel">
          <h3>Commands at this time</h3>
          <CommandList state={view} />
        </section>
        <section className="panel">
          <h3>Events up to this time</h3>
          <ol className="events" data-testid="replay-events">
            {visible.map((e) => (
              <li key={String(e.journalSeq)}>
                <time>{new Date(Number(e.receivedAtUnixMs)).toLocaleTimeString()}</time>{" "}
                {describeEntry(e)}
              </li>
            ))}
          </ol>
        </section>
      </aside>
    </div>
  );
}
