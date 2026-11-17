"""Routability-driven automatic placement (simulated annealing).

KiCad has no automatic placer, so this fills that gap for the generated boards. It is pure Python with
no pcbnew dependency: gen_pcbs.py extracts each footprint's courtyard and pad offsets for the four
orientations and hands them over as plain data.

Cost (lower is better):
- wirelength: Manhattan minimum-spanning-tree length per net over the actual pad positions
  (MST tracks real routing length better than half-perimeter for multi-pin nets). Plane nets (GND)
  are skipped: they drop straight to the In1 plane through a via.
- affinity: distance from each hinted part (decoupling caps, feedback dividers) to the nearest pad of
  its anchor IC on a shared net, so a cap lands at the pin it decouples rather than anywhere on the
  rail.
- congestion: RUDY (rectangular uniform wire density) on a grid; demand above the cell's routing
  capacity is penalised quadratically. This is what keeps parts from piling up into unroutable knots.
- legality: courtyard overlap (with a halo) and leaving the placement area are penalised heavily
  during annealing and removed exactly by a final legalisation pass.

Deterministic for a given seed (CLAUDE.md: no unseeded randomness).
"""

from __future__ import annotations

import math
import random
from dataclasses import dataclass, field

ROTS = (0, 90, 180, 270)


@dataclass
class Part:
    ref: str
    movable: bool
    # per orientation: courtyard (x0, y0, x1, y1) and pad list [(dx, dy, net)] relative to the origin
    shapes: dict[int, tuple[tuple[float, float, float, float], list[tuple[float, float, str]]]]
    x: float
    y: float
    rot: int
    halo: float = 0.4  # extra clearance around the courtyard (routing room)
    anchor: str | None = None  # affinity target (hinted part)


@dataclass
class Problem:
    parts: dict[str, Part]
    area: tuple[float, float, float, float]  # allowed placement rectangle for movable courtyards
    plane_nets: set[str] = field(default_factory=set)
    net_weight: dict[str, float] = field(default_factory=dict)
    grid_mm: float = 2.5
    capacity: float = 1.6  # routing demand one grid cell can absorb (tracks per mm, all layers)
    w_affinity: float = 3.0
    w_congestion: float = 4.0
    w_overlap: float = 200.0


class Placer:
    def __init__(self, prob: Problem, seed: int = 1):
        self.p = prob
        self.rng = random.Random(seed)
        self.parts = prob.parts
        self.refs = list(self.parts)
        self.movable = [r for r in self.refs if self.parts[r].movable]
        self.nets: dict[str, list[tuple[str, int]]] = {}
        for r, part in self.parts.items():
            for i, (_, _, n) in enumerate(part.shapes[0][1]):
                if n and n not in prob.plane_nets:
                    self.nets.setdefault(n, []).append((r, i))
        self.nets = {n: pins for n, pins in self.nets.items() if len({r for r, _ in pins}) > 1}
        self.dependents = {r: [o for o in self.refs if o == r or self.parts[o].anchor == r] for r in self.refs}
        self.part_nets = {r: sorted({n for n, pins in self.nets.items() if any(pr == r for pr, _ in pins)})
                          for r in self.refs}
        x0, y0, x1, y1 = prob.area
        self.gx0, self.gy0 = x0 - 5, y0 - 5
        self.gw = int((x1 - x0 + 10) / prob.grid_mm) + 1
        self.gh = int((y1 - y0 + 10) / prob.grid_mm) + 1
        self.demand = [0.0] * (self.gw * self.gh)
        self.net_len: dict[str, float] = {}
        self.net_box: dict[str, tuple[float, float, float, float]] = {}
        for n in self.nets:
            self._net_update(n, +1)

    # ---- geometry ----------------------------------------------------------------------
    def box(self, r: str, x=None, y=None, rot=None, halo=True):
        part = self.parts[r]
        x = part.x if x is None else x
        y = part.y if y is None else y
        rot = part.rot if rot is None else rot
        bx0, by0, bx1, by1 = part.shapes[rot][0]
        h = part.halo if halo else 0.0
        return (x + bx0 - h, y + by0 - h, x + bx1 + h, y + by1 + h)

    def pin(self, r: str, i: int):
        part = self.parts[r]
        dx, dy, _ = part.shapes[part.rot][1][i]
        return part.x + dx, part.y + dy

    # ---- cost terms --------------------------------------------------------------------
    def _mst(self, pts):
        n = len(pts)
        if n < 2:
            return 0.0
        if n == 2:
            return abs(pts[0][0] - pts[1][0]) + abs(pts[0][1] - pts[1][1])
        dist = [math.inf] * n
        used = [False] * n
        dist[0] = 0.0
        total = 0.0
        for _ in range(n):
            k = min((i for i in range(n) if not used[i]), key=dist.__getitem__)
            used[k] = True
            total += dist[k]
            kx, ky = pts[k]
            for i in range(n):
                if not used[i]:
                    d = abs(kx - pts[i][0]) + abs(ky - pts[i][1])
                    if d < dist[i]:
                        dist[i] = d
        return total

    def _rudy(self, box, sign):
        x0, y0, x1, y1 = box
        w, h = max(x1 - x0, 0.5), max(y1 - y0, 0.5)
        dens = sign * (w + h) / (w * h)
        g = self.p.grid_mm
        for gy in range(max(0, int((y0 - self.gy0) / g)), min(self.gh, int((y1 - self.gy0) / g) + 1)):
            row = gy * self.gw
            for gx in range(max(0, int((x0 - self.gx0) / g)), min(self.gw, int((x1 - self.gx0) / g) + 1)):
                self.demand[row + gx] += dens

    def _net_update(self, n, sign):
        if sign < 0:
            self._rudy(self.net_box[n], -1)
            return
        pts = [self.pin(r, i) for r, i in self.nets[n]]
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        self.net_box[n] = (min(xs), min(ys), max(xs), max(ys))
        self.net_len[n] = self._mst(pts) * self.p.net_weight.get(n, 1.0)
        self._rudy(self.net_box[n], +1)

    def congestion(self) -> float:
        cap = self.p.capacity
        return sum((d - cap) ** 2 for d in self.demand if d > cap)

    def affinity(self, r: str) -> float:
        part = self.parts[r]
        if not part.anchor:
            return 0.0
        a = self.parts[part.anchor]
        shared = {n for _, _, n in part.shapes[0][1]} & {n for _, _, n in a.shapes[0][1]}
        shared -= self.p.plane_nets
        if not shared:
            ax, ay = a.x, a.y
            return abs(part.x - ax) + abs(part.y - ay)
        best = math.inf
        for i, (_, _, n) in enumerate(part.shapes[0][1]):
            if n not in shared:
                continue
            px, py = self.pin(r, i)
            for j, (_, _, m) in enumerate(a.shapes[0][1]):
                if m == n:
                    qx, qy = self.pin(part.anchor, j)
                    best = min(best, abs(px - qx) + abs(py - qy))
        return best

    def overlap_of(self, r: str) -> float:
        b = self.box(r)
        tot = 0.0
        for o in self.refs:
            if o == r:
                continue
            c = self.box(o, halo=False) if not self.parts[o].movable else self.box(o)
            ox = min(b[2], c[2]) - max(b[0], c[0])
            oy = min(b[3], c[3]) - max(b[1], c[1])
            if ox > 0 and oy > 0:
                tot += ox * oy
        x0, y0, x1, y1 = self.p.area
        bb = self.box(r, halo=False)
        out = max(0, x0 - bb[0]) + max(0, bb[2] - x1) + max(0, y0 - bb[1]) + max(0, bb[3] - y1)
        return tot + 10 * out

    def total(self) -> dict[str, float]:
        wl = sum(self.net_len.values())
        af = sum(self.affinity(r) for r in self.refs)
        ov = sum(self.overlap_of(r) for r in self.movable)
        cg = self.congestion()
        return {"wirelength": wl, "affinity": af, "congestion": cg, "overlap": ov,
                "cost": wl + self.p.w_affinity * af + self.p.w_congestion * cg + self.p.w_overlap * ov}

    # ---- annealing ---------------------------------------------------------------------
    def _local_cost(self, rs):
        nets = sorted({n for r in rs for n in self.part_nets[r]})
        wl = sum(self.net_len[n] for n in nets)
        af = sum(self.affinity(o) for o in {d for r in rs for d in self.dependents[r]} | rs)
        ov = sum(self.overlap_of(r) for r in rs)
        return wl + self.p.w_affinity * af + self.p.w_overlap * ov, nets

    def _apply(self, moves, nets):
        for n in nets:
            self._net_update(n, -1)
        for r, (x, y, rot) in moves.items():
            part = self.parts[r]
            part.x, part.y, part.rot = x, y, rot
        for n in nets:
            self._net_update(n, +1)

    def anneal(self, iterations: int = 150_000, t0: float = 4.0, t1: float = 0.02, step0: float = 12.0) -> None:
        rng = self.rng
        x0, y0, x1, y1 = self.p.area
        cong = self.congestion()
        for it in range(iterations):
            frac = it / iterations
            t = t0 * (t1 / t0) ** frac
            step = max(0.5, step0 * (1 - frac))
            r = rng.choice(self.movable)
            part = self.parts[r]
            kind = rng.random()
            if kind < 0.1:  # swap with a similar-sized movable part
                o = rng.choice(self.movable)
                if o == r:
                    continue
                moves = {r: (self.parts[o].x, self.parts[o].y, part.rot),
                         o: (part.x, part.y, self.parts[o].rot)}
            elif kind < 0.2:
                moves = {r: (part.x, part.y, rng.choice(ROTS))}
            else:
                nx = min(max(part.x + rng.gauss(0, step), x0), x1)
                ny = min(max(part.y + rng.gauss(0, step), y0), y1)
                moves = {r: (round(nx * 20) / 20, round(ny * 20) / 20, part.rot)}
            rs = set(moves)
            before, nets = self._local_cost(rs)
            old = {m: (self.parts[m].x, self.parts[m].y, self.parts[m].rot) for m in rs}
            self._apply(moves, nets)
            after, _ = self._local_cost(rs)
            new_cong = self.congestion()
            delta = (after - before) + self.p.w_congestion * (new_cong - cong)
            if delta <= 0 or rng.random() < math.exp(-delta / t):
                cong = new_cong
            else:
                self._apply(old, nets)

    # ---- legalisation ------------------------------------------------------------------
    def legal(self, r: str) -> bool:
        return self.overlap_of(r) == 0.0

    def legalize(self) -> list[str]:
        """Move every illegal part to the cheapest legal spot nearby (spiral search). Returns failures."""
        failed = []
        order = sorted(self.movable, key=lambda r: -self.overlap_of(r))
        for r in order:
            if self.legal(r):
                continue
            part = self.parts[r]
            ox, oy, orot = part.x, part.y, part.rot
            best = None
            for ring in range(1, 160):
                rad = 0.25 * ring
                steps = max(8, int(2 * math.pi * rad / 0.25))
                for k in range(steps):
                    a = 2 * math.pi * k / steps
                    for rot in (orot,) + tuple(x for x in ROTS if x != orot):
                        cand = (round((ox + rad * math.cos(a)) * 20) / 20, round((oy + rad * math.sin(a)) * 20) / 20, rot)
                        nets = self.part_nets[r]
                        self._apply({r: cand}, nets)
                        if self.legal(r):
                            c, _ = self._local_cost({r})
                            if best is None or c < best[0]:
                                best = (c, cand)
                        self._apply({r: (ox, oy, orot)}, nets)
                if best and ring > 4:
                    break
            if best:
                self._apply({r: best[1]}, self.part_nets[r])
            else:
                failed.append(r)
        return failed

    def solution(self) -> dict[str, tuple[float, float, int]]:
        return {r: (p.x, p.y, p.rot) for r, p in self.parts.items() if p.movable}


def optimize(prob: Problem, seed: int = 1, iterations: int = 150_000, log=print) -> dict[str, tuple[float, float, int]]:
    pl = Placer(prob, seed)
    log(f"  place: start {fmt(pl.total())}")
    pl.anneal(iterations)
    log(f"  place: annealed {fmt(pl.total())}")
    failed = pl.legalize()
    if failed:
        raise SystemExit(f"placement could not legalise {failed}")
    final = pl.total()
    log(f"  place: legal {fmt(final)}")
    assert final["overlap"] == 0.0
    return pl.solution()


def fmt(c: dict[str, float]) -> str:
    return ", ".join(f"{k}={v:.1f}" for k, v in c.items())
