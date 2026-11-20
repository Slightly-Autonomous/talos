"""Close the last open connections an autorouter leaves: a small grid maze router (A*) on F.Cu and B.Cu.

Freerouting routes ~99 % of a dense board and then gives up on one or two connections that need a
detour. A full re-route costs another 30-90 min; this searches a window around the open pair on a
0.1 mm grid with both outer layers and vias, treating every other net's copper (inflated by clearance
plus half the track width), keep-out rule areas and the board edge as walls. Same-net copper is free to
cross, so a path may join the net anywhere. The caller re-runs DRC on the result.

Used by route_pcbs.py's repair pass; needs KiCad's Python (pcbnew).
"""

from __future__ import annotations

import heapq
import math

import pcbnew

GRID_MM = 0.1
VIA_COST = 25.0  # in grid steps (2.5 mm): prefer staying on a layer
LAYERS = (pcbnew.F_Cu, pcbnew.B_Cu)


class Window:
    def __init__(self, x0, y0, x1, y1, step):
        self.x0, self.y0, self.step = x0, y0, step
        self.nx = int((x1 - x0) / step) + 1
        self.ny = int((y1 - y0) / step) + 1

    def pt(self, i, j) -> pcbnew.VECTOR2I:
        return pcbnew.VECTOR2I(int(self.x0 + i * self.step), int(self.y0 + j * self.step))

    def cells(self, box):  # cells whose centre may fall in a bounding box
        i0 = max(0, int((box.GetX() - self.x0) / self.step))
        j0 = max(0, int((box.GetY() - self.y0) / self.step))
        i1 = min(self.nx - 1, int((box.GetRight() - self.x0) / self.step) + 1)
        j1 = min(self.ny - 1, int((box.GetBottom() - self.y0) / self.step) + 1)
        for j in range(j0, j1 + 1):
            for i in range(i0, i1 + 1):
                yield i, j


def _blocked(board, win: Window, net: int, layer: int, inflate: int) -> bytearray:
    """Cells on `layer` where a track centre of this net would violate clearance: 1 = wall (pads, vias,
    locked copper, keep-outs, edge), 2 = another net's unlocked track (rip-up may remove it)."""
    grid = bytearray(win.nx * win.ny)
    hard = [p for p in board.GetPads() if p.IsOnLayer(layer)]
    hard += [t for t in board.GetTracks() if t.IsOnLayer(layer) and (t.GetClass() == "PCB_VIA" or t.IsLocked())]
    soft = [t for t in board.GetTracks() if t.IsOnLayer(layer) and t.GetClass() != "PCB_VIA" and not t.IsLocked()]
    # Copper pours are not walls: they refill around whatever is added.
    for value, items in ((1, hard), (2, soft)):
        for it in items:
            if it.GetNetCode() == net and it.GetNetCode() > 0:
                continue
            box = it.GetBoundingBox()
            box.Inflate(inflate)
            shape = it.GetEffectiveShape(layer)
            for i, j in win.cells(box):
                k = j * win.nx + i
                if not grid[k] and shape.Collide(win.pt(i, j), inflate):
                    grid[k] = value
    keepouts = [z for z in board.Zones() if z.GetIsRuleArea() and z.GetDoNotAllowTracks() and z.IsOnLayer(layer)]
    keepouts += [z for fp in board.Footprints() for z in fp.Zones()
                 if z.GetIsRuleArea() and z.GetDoNotAllowTracks() and z.IsOnLayer(layer)]
    for z in keepouts:
        box = z.GetBoundingBox()
        box.Inflate(inflate)
        for i, j in win.cells(box):
            k = j * win.nx + i
            if not grid[k] and z.Outline().Collide(win.pt(i, j), inflate):
                grid[k] = 1
    edge = board.GetBoardEdgesBoundingBox()
    edge.Inflate(-inflate - pcbnew.FromMM(0.3))
    outline = pcbnew.SHAPE_POLY_SET()
    board.GetBoardPolygonOutlines(outline, True)
    for j in range(win.ny):
        for i in range(win.nx):
            p = win.pt(i, j)
            if not edge.Contains(p) or not outline.Contains(p):
                grid[j * win.nx + i] = 1
    return grid


def _inside(item, win: Window, layer: int) -> list[tuple[int, int]]:
    if not item.IsOnLayer(layer):
        return []
    shape = item.GetEffectiveShape(layer)
    return [(i, j) for i, j in win.cells(item.GetBoundingBox()) if shape.Collide(win.pt(i, j), 0)]


RIP_COST = 6.0  # per grid step through another net's track (rip-up mode)


def connect(board, a, b, window_mm: float = 12.0, rip: bool = False) -> bool:
    """Route one track (with vias as needed) joining items a and b of the same net. True if added.

    With rip=True another net's unlocked tracks are passable at a cost; the ones the new path crosses
    are deleted afterwards, and the repair loop's next round re-routes those connections."""
    F = pcbnew.FromMM
    net = a.GetNetCode()
    nc = board.GetDesignSettings().m_NetSettings.GetEffectiveNetClass(a.GetNetname())
    width = max(F(0.1), min(nc.GetTrackWidth(), F(0.2)))
    clear = max(F(0.15), nc.GetClearance()) + F(0.02)
    ds = board.GetDesignSettings()
    via_d, via_drill = max(ds.m_ViasMinSize, F(0.5)), max(ds.m_MinThroughDrill, F(0.3))
    ba, bb = a.GetBoundingBox(), b.GetBoundingBox()
    ba.Merge(bb)
    ba.Inflate(F(window_mm))
    step = F(GRID_MM)
    win = Window(ba.GetX(), ba.GetY(), ba.GetRight(), ba.GetBottom(), step)
    trk = {L: _blocked(board, win, net, L, clear + width // 2) for L in LAYERS}
    via_r = clear + via_d // 2
    viab = {L: _blocked(board, win, net, L, via_r) for L in LAYERS}
    no_via = bytearray(win.nx * win.ny)
    same_via = bytearray(win.nx * win.ny)  # inside a via of this net: change layer there for free
    hole_gap = via_drill + F(0.3)  # drill-to-drill centre spacing (hole_to_hole rule plus margin)
    drilled = [t for t in board.GetTracks() if t.GetClass() == "PCB_VIA"]
    drilled += [q for q in board.GetPads() if q.GetDrillSize().x > 0]
    for d in drilled:
        c = d.GetPosition()
        r = hole_gap if d.GetClass() == "PCB_VIA" else hole_gap + d.GetDrillSize().x // 2
        box = pcbnew.BOX2I(pcbnew.VECTOR2I(c.x - r, c.y - r), pcbnew.VECTOR2L(2 * r, 2 * r))
        for i, j in win.cells(box):
            dist = (win.pt(i, j) - c).EuclideanNorm()
            if dist < r:
                no_via[j * win.nx + i] = 1
            if d.GetClass() == "PCB_VIA" and d.GetNetCode() == net and dist < d.GetWidth(pcbnew.F_Cu) // 2:
                same_via[j * win.nx + i] = 1
    for z in [z for z in board.Zones() if z.GetIsRuleArea() and z.GetDoNotAllowVias()]:
        box = z.GetBoundingBox()
        box.Inflate(via_r)
        for i, j in win.cells(box):
            if z.Outline().Collide(win.pt(i, j), via_r):
                no_via[j * win.nx + i] = 1
    starts = [(i, j, L) for L in LAYERS for i, j in _inside(a, win, L)]
    goals = {(i, j, L) for L in LAYERS for i, j in _inside(b, win, L)}
    if not starts or not goals:
        return False
    gx = sum(g[0] for g in goals) / len(goals)
    gy = sum(g[1] for g in goals) / len(goals)

    def h(i, j):
        dx, dy = abs(i - gx), abs(j - gy)
        return max(dx, dy) + (math.sqrt(2) - 1) * min(dx, dy)

    dist, prev, heap = {}, {}, []
    for s in starts:
        dist[s] = 0.0
        heapq.heappush(heap, (h(s[0], s[1]), 0.0, s))
    moves = [(1, 0, 1.0), (-1, 0, 1.0), (0, 1, 1.0), (0, -1, 1.0),
             (1, 1, math.sqrt(2)), (1, -1, math.sqrt(2)), (-1, 1, math.sqrt(2)), (-1, -1, math.sqrt(2))]
    end = None
    expanded = 0
    while heap and expanded < 2_000_000:
        _, d, cur = heapq.heappop(heap)
        if d > dist.get(cur, math.inf):
            continue
        expanded += 1
        if cur in goals:
            end = cur
            break
        i, j, L = cur
        nbrs = []
        for di, dj, c in moves:
            ni, nj = i + di, j + dj
            if 0 <= ni < win.nx and 0 <= nj < win.ny:
                k = nj * win.nx + ni
                cell = trk[L][k]
                if not cell or (ni, nj, L) in goals:
                    nbrs.append(((ni, nj, L), c))
                elif rip and cell == 2:
                    nbrs.append(((ni, nj, L), c + RIP_COST))
        k = j * win.nx + i
        va, vb = viab[LAYERS[0]][k], viab[LAYERS[1]][k]
        if same_via[k]:  # an existing via of this net: use it instead of drilling another
            nbrs.append(((i, j, LAYERS[1] if L == LAYERS[0] else LAYERS[0]), 0.0))
        elif not no_via[k] and (va, vb) == (0, 0):
            nbrs.append(((i, j, LAYERS[1] if L == LAYERS[0] else LAYERS[0]), VIA_COST))
        elif rip and not no_via[k] and not same_via[k] and va != 1 and vb != 1:
            nbrs.append(((i, j, LAYERS[1] if L == LAYERS[0] else LAYERS[0]), VIA_COST + 4 * RIP_COST))
        for n, c in nbrs:
            nd = d + c
            if nd < dist.get(n, math.inf):
                dist[n] = nd
                prev[n] = cur
                heapq.heappush(heap, (nd + h(n[0], n[1]), nd, n))
    if end is None:
        best = min(dist, key=lambda n: h(n[0], n[1]))
        print(f"    maze: no path for {a.GetNetname()} ({expanded} cells searched, closest "
              f"{h(best[0], best[1]) * GRID_MM:.1f} mm from the target)")
        return False
    path = [end]
    while path[-1] in prev:
        path.append(prev[path[-1]])
    path.reverse()
    # Collapse runs of the same direction into straight segments; a layer change becomes a via.
    pts = [path[0]]
    for p, q, r in zip(path, path[1:], path[2:]):
        if p[2] != q[2] or q[2] != r[2] or (q[0] - p[0], q[1] - p[1]) != (r[0] - q[0], r[1] - q[1]):
            pts.append(q)
    pts.append(path[-1])
    added = []
    for p, q in zip(pts, pts[1:]):
        if p[2] != q[2] and same_via[p[1] * win.nx + p[0]]:
            continue  # layer change through an existing via
        if p[2] != q[2]:
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(win.pt(p[0], p[1]))
            v.SetWidth(via_d)
            v.SetDrill(via_drill)
            v.SetNetCode(net)
            v.SetLocked(True)
            board.Add(v)
            added.append(v)
            continue
        if (p[0], p[1]) == (q[0], q[1]):
            continue
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(win.pt(p[0], p[1]))
        t.SetEnd(win.pt(q[0], q[1]))
        t.SetWidth(width)
        t.SetLayer(p[2])
        t.SetNetCode(net)
        t.SetLocked(True)  # a later rip-up must go around it: stops two nets ripping each other forever
        board.Add(t)
        added.append(t)
    if rip:  # remove the other nets' tracks the new copper now violates
        victims = {}  # uuid -> track (SWIG objects are not hashable)
        for t in board.GetTracks():
            if t.GetNetCode() == net or t.GetClass() == "PCB_VIA" or t.IsLocked():
                continue
            for n in added:
                L = n.GetLayer() if n.GetClass() != "PCB_VIA" else t.GetLayer()
                if t.IsOnLayer(L) and n.GetEffectiveShape(L).Collide(t.GetEffectiveShape(L), clear - F(0.02)):
                    victims[t.m_Uuid.AsString()] = t
                    break
        for t in victims.values():
            board.Delete(t)
        print(f"    maze: {a.GetNetname()} routed through {len(victims)} other track segment(s), ripped for re-route")
    return True


def tidy(board) -> int:
    """Remove what rip-up leaves behind: track segments with a free end (repeatedly), and vias that
    connect nothing. Returns the number of items removed."""
    removed = 0
    while True:
        tracks = [t for t in board.GetTracks() if t.GetClass() == "PCB_TRACK"]
        vias = [t for t in board.GetTracks() if t.GetClass() == "PCB_VIA"]
        pads = list(board.GetPads())
        zones = [z for z in board.Zones() if not z.GetIsRuleArea()]
        dead = []
        for t in tracks:
            if t.IsLocked() and t.GetLength() == 0:
                dead.append(t)
                continue
            for end in (t.GetStart(), t.GetEnd()):
                net, L = t.GetNetCode(), t.GetLayer()
                hit = any(o is not t and o.GetNetCode() == net and o.IsOnLayer(L) and
                          o.GetEffectiveShape(L).Collide(end, t.GetWidth() // 2) for o in tracks + vias)
                hit = hit or any(p.GetNetCode() == net and p.IsOnLayer(L) and p.GetEffectiveShape(L).Collide(end, 0)
                                 for p in pads)
                hit = hit or any(z.GetNetCode() == net and z.GetLayer() == L and z.GetFilledPolysList(L).Contains(end)
                                 for z in zones)
                if not hit:
                    dead.append(t)
                    break
        if not dead:
            return removed
        for t in dead:
            board.Delete(t)
        removed += len(dead)
