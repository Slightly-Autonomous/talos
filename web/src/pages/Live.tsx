import { useEffect, useState } from "react";
import { HubError } from "@talos/contracts/client";
import { ArmMode } from "@talos/contracts/talos/v1/rule_pb";
import { FloorPlan } from "../components/FloorPlan";
import { useHub } from "../hub/HubProvider";
import { CommandList } from "../components/CommandList";
import {
  armLabel,
  describeEntry,
  deviceHealth,
  deviceTone,
  formatRemaining,
  maintenanceFor,
  tamperLabel,
  tamperStatus,
} from "../model/view";
import { endMaintenanceCommandRequest, maintenanceCommandRequest } from "../model/commands";
import type { Health } from "@talos/contracts/talos/v1/health_pb";
import { HealthStatus } from "@talos/contracts/talos/v1/health_pb";
import { CommandStatus } from "@talos/contracts/talos/v1/command_pb";

// Hub health projection (ADR 0014), refreshed periodically.
function useDeviceHealth(): Map<string, Health> {
  const { client } = useHub();
  const [health, setHealth] = useState<Map<string, Health>>(new Map());
  useEffect(() => {
    let stop = false;
    const load = () =>
      client
        .deviceHealth()
        .then((r) => !stop && setHealth(new Map(r.devices.map((h) => [h.deviceId, h]))))
        .catch(() => {});
    void load();
    const t = setInterval(load, 5000);
    return () => {
      stop = true;
      clearInterval(t);
    };
  }, [client]);
  return health;
}

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
  const [maintError, setMaintError] = useState<string>();

  async function maintenance(deviceId: string, start: boolean) {
    setMaintError(undefined);
    try {
      const r = await client.requestCommand(
        start ? maintenanceCommandRequest(deviceId) : endMaintenanceCommandRequest(deviceId),
      );
      if (r.status === CommandStatus.DENIED) {
        setMaintError(`Maintenance refused: ${r.reason}`);
      }
    } catch (e) {
      setMaintError(e instanceof HubError ? e.message : String(e));
    }
  }
  const health = useDeviceHealth();

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
          {maintError && (
            <p role="alert" className="error" data-testid="maint-error">
              {maintError}
            </p>
          )}
          <ul className="devices">
            {property.devices.map((d) => {
              const projected = health.get(d.id);
              const fallback = deviceHealth(state, d.id);
              const tamper = tamperStatus(property, state, d.id);
              const maint = maintenanceFor(state, d.id, now);
              const [label, tone] = deviceTone(
                projected?.status,
                fallback,
                tamper?.display === "open",
                maint !== undefined || projected?.status === HealthStatus.MAINTENANCE,
              );
              return (
                <li key={d.id} data-testid={`health-${d.id}`}>
                  <span className={`dot dot-${tone}`} aria-hidden /> {d.id}
                  <span className="muted"> · {label}</span>
                  {tamper && (
                    <div
                      className={tamper.display === "open" ? "error small" : "muted small"}
                      data-testid={`tamper-${d.id}`}
                      data-tamper={tamper.display}
                      role={tamper.display === "open" ? "alert" : undefined}
                    >
                      {tamperLabel(tamper)}
                    </div>
                  )}
                  {tamper && (
                    <div className="small">
                      {maint ? (
                        <>
                          <span data-testid={`maint-${d.id}`} data-maintenance="on">
                            maintenance: tamper alarm off for {formatRemaining(maint.remainingMs)}
                          </span>{" "}
                          <button
                            data-testid={`maint-end-${d.id}`}
                            onClick={() => void maintenance(d.id, false)}
                          >
                            End maintenance
                          </button>
                        </>
                      ) : (
                        <button
                          data-testid={`maint-start-${d.id}`}
                          onClick={() => void maintenance(d.id, true)}
                        >
                          Maintenance (30 min)
                        </button>
                      )}
                    </div>
                  )}
                  {projected?.recommendedAction && (
                    <div className="muted small">
                      {projected.evidence.join("; ")} · {projected.recommendedAction}
                    </div>
                  )}
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
