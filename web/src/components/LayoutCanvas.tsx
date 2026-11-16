// Editable plan canvas: draws a draft property on a 1 m grid and reports
// clicks in property coordinates (meters, +y away from the street).
import type { MouseEvent } from "react";
import type { Point, Property } from "@talos/contracts/talos/v1/property_pb";
import { bounds } from "../model/view";

interface Props {
  property: Property;
  selectedZone?: string;
  selectedDevice?: string;
  selectedAsset?: string;
  onClick: (p: { x: number; y: number }) => void;
}

const PAD = 3;

export function LayoutCanvas({
  property,
  selectedZone,
  selectedDevice,
  selectedAsset,
  onClick,
}: Props) {
  const b = bounds(property);
  const minX = Math.floor(b.minX) - PAD;
  const maxX = Math.ceil(b.maxX) + PAD;
  const minY = Math.floor(b.minY) - PAD;
  const maxY = Math.ceil(b.maxY) + PAD;
  const X = (x: number) => x - minX;
  const Y = (y: number) => maxY - y;
  const pts = (ps: Point[]) => ps.map((p) => `${X(p.x)},${Y(p.y)}`).join(" ");

  function handle(e: MouseEvent<SVGSVGElement>) {
    const svg = e.currentTarget;
    const ctm = svg.getScreenCTM();
    if (!ctm) return;
    const pt = new DOMPoint(e.clientX, e.clientY).matrixTransform(ctm.inverse());
    onClick({ x: pt.x + minX, y: maxY - pt.y });
  }

  const grid = [];
  for (let x = minX; x <= maxX; x++) {
    grid.push(<line key={`gx${x}`} className="grid" x1={X(x)} y1={0} x2={X(x)} y2={maxY - minY} />);
  }
  for (let y = minY; y <= maxY; y++) {
    grid.push(<line key={`gy${y}`} className="grid" x1={0} y1={Y(y)} x2={maxX - minX} y2={Y(y)} />);
  }

  return (
    <svg
      className="floorplan editor"
      viewBox={`0 0 ${maxX - minX} ${maxY - minY}`}
      onClick={handle}
      data-testid="layout-canvas"
      role="application"
      aria-label="Layout editor canvas"
    >
      {grid}
      {property.zones.map((z) => (
        <g key={z.id} data-testid={`edit-zone-${z.id}`}>
          <polygon
            points={pts(z.polygon)}
            className={`zone ${z.id === selectedZone ? "zone-selected" : "zone-unknown"}`}
          />
          {z.id === selectedZone &&
            z.polygon.map((p, i) => (
              <circle key={i} cx={X(p.x)} cy={Y(p.y)} r={0.18} className="vertex" />
            ))}
          {z.polygon.length > 0 && (
            <text
              x={X(z.polygon.reduce((s, p) => s + p.x, 0) / z.polygon.length)}
              y={Y(z.polygon.reduce((s, p) => s + p.y, 0) / z.polygon.length)}
              className="zone-label"
            >
              {z.name || z.id}
            </text>
          )}
        </g>
      ))}
      {property.walls.map((w, i) =>
        w.a && w.b ? (
          <line key={i} className="wall" x1={X(w.a.x)} y1={Y(w.a.y)} x2={X(w.b.x)} y2={Y(w.b.y)} />
        ) : null,
      )}
      {property.assets.map((a) =>
        a.segment?.a && a.segment.b ? (
          <line
            key={a.id}
            className={`portal ${a.id === selectedAsset ? "portal-open" : "portal-closed"}`}
            x1={X(a.segment.a.x)}
            y1={Y(a.segment.a.y)}
            x2={X(a.segment.b.x)}
            y2={Y(a.segment.b.y)}
          />
        ) : null,
      )}
      {property.devices.map((d) =>
        d.position ? (
          <g key={d.id}>
            <circle
              cx={X(d.position.x)}
              cy={Y(d.position.y)}
              r={d.id === selectedDevice ? 0.5 : 0.35}
              className={
                d.id === selectedDevice ? "device device-selected" : "device device-unknown"
              }
            />
            <text x={X(d.position.x) + 0.55} y={Y(d.position.y) + 0.25} className="device-label">
              {d.id}
            </text>
          </g>
        ) : null,
      )}
    </svg>
  );
}
