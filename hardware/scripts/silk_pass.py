"""Silkscreen finishing pass for a placed (or routed) board: legible, non-overlapping reference designators.

Run with KiCad's bundled Python after routing:
    python3 hardware/scripts/silk_pass.py hardware/kicad/<board>/<board>.kicad_pcb [...]

For each footprint the reference is tried at spots around its courtyard (above, below, left, right,
corners, one step further out). The first spot that is on the board, off every exposed pad, and clear of
other silkscreen wins. A part with no free spot keeps its reference on the fab layer only (the assembly
drawing still shows it), which is what a hand layout does on a dense board. Text on the back silk is
mirrored so it reads correctly from the bottom. Deterministic: footprints are handled in reference order.
"""

from __future__ import annotations

import re
import sys

import pcbnew

CLEAR_MM = 0.15  # silk to silk / silk to pad
EDGE_MM = 0.3
SIZES_MM = (0.8,)  # the project minimum text height is 0.8 mm
MIN_THICK_MM = 0.12  # JLCPCB minimum silk line width is 0.15 at 1 mm, 0.1 absolute; keep 0.12


def mm(v: float) -> int:
    return pcbnew.FromMM(v)


class Grid:
    """Bucketed boxes for fast overlap queries."""

    def __init__(self, cell_mm: float = 3.0):
        self.cell = mm(cell_mm)
        self.b: dict[tuple[int, int], list] = {}

    def _keys(self, box):
        x0, y0, x1, y1 = box
        for gx in range(x0 // self.cell, x1 // self.cell + 1):
            for gy in range(y0 // self.cell, y1 // self.cell + 1):
                yield gx, gy

    def add(self, box, tag=None):
        for k in self._keys(box):
            self.b.setdefault(k, []).append((box, tag))

    def hits(self, box, ignore=None) -> bool:
        x0, y0, x1, y1 = box
        for k in self._keys(box):
            for (a0, b0, a1, b1), tag in self.b.get(k, ()):
                if ignore is not None and tag == ignore:
                    continue
                if a0 < x1 and x0 < a1 and b0 < y1 and y0 < b1:
                    return True
        return False


def _box(bb, grow=0):
    return (bb.GetX() - grow, bb.GetY() - grow, bb.GetRight() + grow, bb.GetBottom() + grow)


def courtyard(fp):
    box = None
    for layer in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
        poly = fp.GetCourtyard(layer)
        if poly.OutlineCount():
            bb = poly.BBox()
            box = _box(bb) if box is None else (min(box[0], bb.GetX()), min(box[1], bb.GetY()),
                                                  max(box[2], bb.GetRight()), max(box[3], bb.GetBottom()))
    return box or _box(fp.GetBoundingBox(False))


def finish(path: str) -> dict:
    board = pcbnew.LoadBoard(path)
    edge = board.GetBoardEdgesBoundingBox()
    ex0, ey0, ex1, ey1 = _box(edge, -mm(EDGE_MM))
    outline = pcbnew.SHAPE_POLY_SET()
    board.GetBoardPolygonOutlines(outline, True)
    sides = {pcbnew.F_SilkS: Grid(), pcbnew.B_SilkS: Grid()}
    pads = {pcbnew.F_SilkS: Grid(), pcbnew.B_SilkS: Grid()}
    fps = sorted(board.GetFootprints(), key=lambda f: (re.sub(r"\d", "", f.GetReference()),
                                                       int(re.sub(r"\D", "", f.GetReference()) or 0)))
    for fp in fps:
        for pad in fp.Pads():
            for silk, cu in ((pcbnew.F_SilkS, pcbnew.F_Cu), (pcbnew.B_SilkS, pcbnew.B_Cu)):
                if pad.IsOnLayer(cu):
                    pads[silk].add(_box(pad.GetBoundingBox(), mm(CLEAR_MM)))
        for g in fp.GraphicalItems():  # footprint outlines stay; references must avoid them
            if g.GetLayer() in sides:
                sides[g.GetLayer()].add(_box(g.GetBoundingBox(), mm(CLEAR_MM)), fp.GetReference())
        for f in fp.GetFields():  # visible value/user fields on silk are obstacles too
            if f.GetLayer() in sides and f.IsVisible() and not f.IsReference():
                sides[f.GetLayer()].add(_box(f.GetBoundingBox(), mm(CLEAR_MM)), fp.GetReference())
    for d in board.GetDrawings():  # board-level text (name, revision, S/N box)
        if d.GetLayer() in sides:
            sides[d.GetLayer()].add(_box(d.GetBoundingBox(), mm(CLEAR_MM)))
    placed = hidden = 0
    for fp in fps:
        ref = fp.Reference()
        layer = pcbnew.B_SilkS if fp.GetLayer() == pcbnew.B_Cu else pcbnew.F_SilkS
        if fp.IsBoardOnly() and fp.GetReference().startswith("FID"):
            ref.SetVisible(False)
            continue
        ref.SetLayer(layer)
        ref.SetMirrored(layer == pcbnew.B_SilkS)
        ref.SetKeepUpright(True)
        x0, y0, x1, y1 = courtyard(fp)
        pb = None
        for pad in fp.Pads():
            b = _box(pad.GetBoundingBox())
            pb = b if pb is None else (min(pb[0], b[0]), min(pb[1], b[1]), max(pb[2], b[2]), max(pb[3], b[3]))
        if pb and (x1 - x0) * (y1 - y0) > 2 * (pb[2] - pb[0]) * (pb[3] - pb[1]):
            x0, y0, x1, y1 = pb  # a module whose courtyard includes an antenna keepout: label by its pads
        cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
        done = False
        for size in SIZES_MM:
            s = mm(size)
            ref.SetTextSize(pcbnew.VECTOR2I(s, s))
            ref.SetTextThickness(max(mm(MIN_THICK_MM), int(s * 0.15)))
            gap = mm(0.25) + s // 2
            spots = []
            if x1 - x0 > mm(6) and y1 - y0 > mm(6):  # modules, ICs: the reference sits on the body
                spots += [(cx, cy, 0)]
            for k in (1, 2, 3):  # rings further out, both orientations on every side
                g = gap * k
                spots += [(cx, y0 - g, 0), (cx, y1 + g, 0), (x0 - g, cy, 90), (x1 + g, cy, 90),
                          (x0 - g, cy, 0), (x1 + g, cy, 0), (cx, y0 - g, 90), (cx, y1 + g, 90),
                          (x0, y0 - g, 0), (x1, y0 - g, 0), (x0, y1 + g, 0), (x1, y1 + g, 0),
                          (x0 - g, y0, 90), (x0 - g, y1, 90), (x1 + g, y0, 90), (x1 + g, y1, 90)]
            for x, y, ang in spots:
                ref.SetTextAngle(pcbnew.EDA_ANGLE(ang, pcbnew.DEGREES_T))
                ref.SetPosition(pcbnew.VECTOR2I(int(x), int(y)))
                ref.SetVisible(True)
                b = _box(ref.GetBoundingBox())
                if b[0] < ex0 or b[1] < ey0 or b[2] > ex1 or b[3] > ey1:
                    continue
                if not all(outline.Contains(pcbnew.VECTOR2I(px, py)) for px, py in
                           ((b[0], b[1]), (b[2], b[1]), (b[0], b[3]), (b[2], b[3]))):
                    continue
                on_body = (x, y) == (cx, cy)  # on the body its own outline is not in the way
                if pads[layer].hits(b) or sides[layer].hits(b, ignore=fp.GetReference() if on_body else None):
                    continue
                sides[layer].add(_box(ref.GetBoundingBox(), mm(CLEAR_MM)))
                done = True
                break
            if done:
                break
        if done:
            placed += 1
        else:
            ref.SetVisible(False)  # the fab layer value/ref still documents it
            hidden += 1
    for d in board.GetDrawings():  # board text on the back reads from the back
        if d.GetLayer() == pcbnew.B_SilkS and d.GetClass() == "PCB_TEXT":
            d.SetMirrored(True)
    board.Save(path)
    return {"placed": placed, "fab_only": hidden}


def main() -> int:
    for p in sys.argv[1:]:
        r = finish(p)
        print(f"{p}: {r['placed']} references on silk, {r['fab_only']} on fab only")
    return 0


if __name__ == "__main__":
    sys.exit(main())
