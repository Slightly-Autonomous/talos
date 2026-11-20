// SVG floor plan. Zones are shaded by region-level evidence (never fake XY
// tracks), portals show open/closed/locked, devices show health, and nominal
// sensor coverage is drawn faintly.
import type { StateResponse } from "@talos/contracts/talos/api/v1/api_pb";
import type { Point, Property } from "@talos/contracts/talos/v1/property_pb";
import {
  bounds,
  deviceHealth,
  portalDisplay,
  portalLabel,
  portalShowsLocked,
  portalStatus,
  wedgePath,
  zoneActivity,
  type ZoneActivity,
} from "../model/view";

interface Props {
  property: Property;
  state: StateResponse | undefined;
  nowMs: number;
  heat?: Map<string, number>;
}

const PAD = 1.5;

export function FloorPlan({ property, state, nowMs, heat }: Props) {
  const b = bounds(property);
  const w = b.maxX - b.minX + 2 * PAD;
  const h = b.maxY - b.minY + 2 * PAD;
  // Property y points "up/away"; SVG y points down.
  const X = (x: number) => x - b.minX + PAD;
  const Y = (y: number) => b.maxY - y + PAD;
  const pts = (ps: Point[]) => ps.map((p) => `${X(p.x)},${Y(p.y)}`).join(" ");
  const activity = zoneActivity(property, state, nowMs);
  const portals = portalStatus(property, state);
  const maxHeat = heat ? Math.max(1, ...heat.values()) : 1;

  return (
    <svg
      className="floorplan"
      viewBox={`0 0 ${w} ${h}`}
      role="img"
      aria-label={`Floor plan of ${property.id}`}
    >
      {property.zones.map((z) => {
        const a: ZoneActivity = activity.get(z.id) ?? "unknown";
        const c = z.polygon.reduce((acc, p) => ({ x: acc.x + p.x, y: acc.y + p.y }), {
          x: 0,
          y: 0,
        });
        const n = Math.max(1, z.polygon.length);
        const heatLevel = heat ? (heat.get(z.id) ?? 0) / maxHeat : undefined;
        return (
          <g key={z.id} data-testid={`zone-${z.id}`} data-activity={a}>
            <polygon
              points={pts(z.polygon)}
              className={`zone zone-${a}`}
              style={heatLevel !== undefined ? { fillOpacity: 0.08 + 0.7 * heatLevel } : undefined}
            />
            <text x={X(c.x / n)} y={Y(c.y / n)} className="zone-label">
              {z.name || z.id}
              {heat ? ` · ${heat.get(z.id) ?? 0}` : ""}
            </text>
          </g>
        );
      })}
      {property.devices.flatMap((d) =>
        d.position
          ? d.coverage.map((cov, i) => (
              <path
                key={`${d.id}-cov-${i}`}
                className="coverage"
                d={wedgePath(
                  X(d.position!.x),
                  Y(d.position!.y),
                  -cov.headingDeg,
                  cov.halfAngleDeg,
                  cov.rangeM,
                )}
              />
            ))
          : [],
      )}
      {property.walls.map((wall, i) =>
        wall.a && wall.b ? (
          <line
            key={`wall-${i}`}
            className="wall"
            x1={X(wall.a.x)}
            y1={Y(wall.a.y)}
            x2={X(wall.b.x)}
            y2={Y(wall.b.y)}
          />
        ) : null,
      )}
      {property.assets.map((asset) => {
        const s = asset.segment;
        if (!s?.a || !s.b) return null;
        const st = portals.get(asset.id);
        const shown = portalDisplay(st);
        const locked = portalShowsLocked(st);
        // data-open/data-locked carry what is shown: a stale report is
        // "unknown", never its last value. data-last-* keep that for debugging.
        return (
          <g
            key={asset.id}
            data-testid={`portal-${asset.id}`}
            data-open={shown === "unknown" ? "unknown" : String(shown === "open")}
            data-locked={st?.locked === undefined || st.lockedStale ? "unknown" : String(locked)}
            data-stale={String(!!st?.stale)}
            data-last-open={String(st?.open)}
            data-last-locked={String(st?.locked)}
          >
            <line
              className={`portal portal-${shown} ${locked ? "portal-locked" : ""}`}
              x1={X(s.a.x)}
              y1={Y(s.a.y)}
              x2={X(s.b.x)}
              y2={Y(s.b.y)}
            >
              <title>
                {asset.id}: {portalLabel(st)}
              </title>
            </line>
          </g>
        );
      })}
      {property.devices.map((d) =>
        d.position ? (
          <g key={d.id} data-testid={`device-${d.id}`}>
            <circle
              cx={X(d.position.x)}
              cy={Y(d.position.y)}
              r={0.35}
              className={`device device-${deviceHealth(state, d.id)}`}
            >
              <title>
                {d.id}: {deviceHealth(state, d.id)}
              </title>
            </circle>
            <text x={X(d.position.x) + 0.5} y={Y(d.position.y) + 0.25} className="device-label">
              {d.id}
            </text>
          </g>
        ) : null,
      )}
    </svg>
  );
}
