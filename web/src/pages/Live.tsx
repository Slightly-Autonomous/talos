import { useEffect, useState } from "react";
import { HubError } from "@talos/contracts/client";
import { ArmMode } from "@talos/contracts/talos/v1/rule_pb";
import { FloorPlan } from "../components/FloorPlan";
import { useHub } from "../hub/HubProvider";
import { CommandList } from "../components/CommandList";
import { armLabel, describeEntry, deviceHealth } from "../model/view";

export function useNow(intervalMs = 1000): number {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), intervalMs);
    return () => clearInterval(t);
  }, [intervalMs]);
  return now;
}

export function Live() {
  const { client, property, state, entries } = useHub();
  const now = useNow();
  const [armError, setArmError] = useState<string>();

  async function arm(mode: ArmMode) {
    setArmError(undefined);
    try {
      await client.setArmMode(mode);
    } catch (e) {
      setArmError(e instanceof HubError ? e.message : String(e));
    }
  }

  if (!property) return <p className="muted">Loading property…</p>;
  return (
    <div className="live">
      <section className="panel plan-panel">
        <div className="panel-head">
          <h2>{property.id}</h2>
          <div className="arm">
            <span data-testid="arm-mode" className="arm-mode">
              {armLabel(state?.armMode)}
            </span>
            <button onClick={() => arm(ArmMode.DISARMED)} data-testid="disarm">
              Disarm
            </button>
            <button onClick={() => arm(ArmMode.ARMED_HOME)}>Arm home</button>
            <button onClick={() => arm(ArmMode.ARMED_AWAY)} data-testid="arm-away">
              Arm away
            </button>
          </div>
        </div>
        {armError && (
          <p role="alert" className="error">
            Arming refused: {armError}
          </p>
        )}
        <FloorPlan property={property} state={state} nowMs={now} />
        <Legend />
      </section>
      <aside className="side">
        <section className="panel">
          <h3>Commands</h3>
          <CommandList state={state} />
        </section>
        <section className="panel">
          <h3>Devices</h3>
          <ul className="devices">
            {property.devices.map((d) => {
              const h = deviceHealth(state, d.id);
              return (
                <li key={d.id}>
                  <span className={`dot dot-${h}`} aria-hidden /> {d.id}
                  <span className="muted">
                    {" "}
                    · {h === "fresh" ? "reporting" : h === "stale" ? "stale" : "no reports"}
                  </span>
                </li>
              );
            })}
          </ul>
        </section>
        <section className="panel">
          <h3>Recent events</h3>
          <ol className="events" data-testid="recent-events">
            {[...entries]
              .reverse()
              .slice(0, 12)
              .map((e) => (
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

function Legend() {
  return (
    <p className="legend muted">
      <span className="swatch zone-active" /> motion now <span className="swatch zone-recent" />{" "}
      motion in last 30 s <span className="swatch zone-quiet" /> no recent motion{" "}
      <span className="swatch zone-unknown" /> unknown (no fresh sensor). Zones are regions: a PIR
      cannot say where in the zone someone is.
    </p>
  );
}
