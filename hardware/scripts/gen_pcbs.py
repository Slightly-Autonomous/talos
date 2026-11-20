"""Generate placed (not yet routed) KiCad PCBs from the generated schematics' intended netlists.

Run with KiCad's bundled Python (it provides `pcbnew`):

    ~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 \
        hardware/scripts/gen_pcbs.py

Writes hardware/kicad/<board>/<board>.kicad_pcb. Placement is rule-based: connectors on board edges,
the radio module at the antenna edge, ICs at fixed anchors, and every passive packed next to the IC it
shares the most nets with. Footprints are linked to their schematic symbols (same UUIDs as
gen_schematics.py) so `kicad-cli pcb drc --schematic-parity` works.
"""

from __future__ import annotations

import json
import math
import os
import pathlib
import sys
import uuid

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import place_opt  # noqa: E402
from talos_footprints import (CARD_TONGUE_DEPTH, CARD_TONGUE_WIDTH, HSEC8_EDGE_Y,  # noqa: E402
                              HSEC8_PIN1_X, HSEC8_PITCH, HSEC8_SLOT)

ROOT = pathlib.Path(__file__).resolve().parents[2]
KICAD = ROOT / "hardware" / "kicad"
TALOS_FP = KICAD / "libraries" / "talos-footprints.pretty"
STOCK_FP = pathlib.Path(os.environ.get(
    "KICAD_FOOTPRINT_DIR", os.path.expanduser("~/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints")))
NS = uuid.UUID("6f6b0a52-6a35-4d7e-9f0e-7a1c9e3d2b10")  # must match gen_schematics.py
ORIGIN = (100.0, 100.0)  # board origin on the KiCad page, mm
PAD_ALIASES = {"S1": "SH"}  # footprint pad -> symbol pin (USB-C shield)


def mm(v: float) -> int:
    return pcbnew.FromMM(v)


def pt(x: float, y: float) -> pcbnew.VECTOR2I:
    return pcbnew.VECTOR2I(mm(ORIGIN[0] + x), mm(ORIGIN[1] + y))


def uid(*parts: str) -> str:
    return str(uuid.uuid5(NS, "/".join(parts)))


# ---- board specs ------------------------------------------------------------------------
# Each spec: size (w, h) mm, copper layers, fixed placements {ref: (x, y, rot_deg)} in board coords.
def sn_spec() -> dict:
    w, h = 100.0, 80.0
    cx = w / 2
    fixed = {
        "U1": (cx, 10.7, 0),  # module top edge on the board edge; antenna strip y=0..5.1 kept clear
        # four slots: two on the left edge, two on the right edge; socket body outside the edge
        "J4": (1.0, 28.0, 270), "J5": (1.0, 54.0, 270),
        "J6": (w - 1.0, 28.0, 90), "J7": (w - 1.0, 54.0, 90),
        # power entry along the bottom edge
        "J1": (14.0, h - 4.0, 0), "J2": (36.0, h - 5.0, 0), "J3": (86.0, h - 5.0, 0),
        "U2": (24.0, h - 10.0, 0), "U3": (36.0, h - 18.0, 0), "U4": (52.0, h - 18.0, 0),
        "U5": (68.0, h - 18.0, 0),
        "U14": (cx, 30.0, 0), "U15": (32.0, 40.0, 90), "U16": (68.0, 40.0, 90), "U17": (cx, 48.0, 0),
    }
    for n in range(4):
        y = 28.0 if n % 2 == 0 else 54.0
        fixed[f"U{6 + n}"] = (15.0 if n < 2 else w - 15.0, y - 6.0, 0)
        fixed[f"U{10 + n}"] = (15.0 if n < 2 else w - 15.0, y + 6.0, 0)
    fixed["D4"], fixed["D5"] = (7.0, 18.0, 0), (7.0, 44.0, 0)
    fixed["D6"], fixed["D7"] = (w - 7.0, 18.0, 0), (w - 7.0, 44.0, 0)
    near = {r: "U1" for r in ("C1", "C2", "C3", "R1", "R2", "R3", "R4", "D1", "SW1", "SW2", "R7", "R8", "C4")}
    near.update({r: "U3" for r in ("C5", "C6", "C7", "C8", "R12", "R13", "R14", "R15")})
    near.update({r: "U4" for r in ("C9", "C10", "R16", "R17")})
    near.update({r: "U5" for r in ("C11", "C12", "L1", "R18", "R19", "R20")})
    near.update({"C21": "U14", "C22": "U15", "C23": "U16", "C24": "U17", "R10": "J1", "R11": "J1", "D2": "J2",
                 "D3": "J2"})
    # Only the parts whose position is set by the enclosure/RF/edge are pinned; every other fixed entry
    # is just a starting point for the placement optimiser (place_opt.py).
    pinned = {"U1", "J1", "J2", "J3", "J4", "J5", "J6", "J7"}
    return {"size": (w, h), "layers": 4, "slot_sockets": ["J4", "J5", "J6", "J7"], "fixed": fixed, "keepout_top": 5.2, "near": near, "gap": 1.0,
            "pinned": pinned, "optimize": True,
            # The 41-pin core rail is a plane: the autorouter could not build that tree as tracks.
            "plane_nets": {pcbnew.In2_Cu: "V3V3_CORE"}}


def card_spec(board: str) -> dict:
    w, h = {"TL-MOD-002": (18.0, 30.0), "TL-TEST-001": (22.0, 32.0)}.get(board, (16.0, 26.0))
    fixed = {"J1": (w / 2, h - 1.65, 0)}  # fingers stop 0.15 mm short of the bevelled bottom edge
    # TL-MOD-001: the Hall switch sits at the far edge (opposite the fingers), nearest the door magnet.
    # Its sensitive axis is normal to the package top, so the card's orientation in the enclosure
    # matters as much as this distance (HD-020).
    fixed["U2"] = (w / 2, {"TL-MOD-001": 2.6, "TL-MOD-002": 9.0, "TL-TEST-001": 11.0}.get(board, 8.0), 0)
    near = {"C1": "U1", "C2": "U2", "C3": "U2"}  # decoupling next to the part it decouples
    if board == "TL-MOD-001":
        near.update({"C3": "U3", "R1": "U2"})
    # Only a 9.25 mm tongue enters the HSEC8 socket; the card is cut back either side of it.
    half = CARD_TONGUE_WIDTH / 2
    cuts = [(0.0, h - CARD_TONGUE_DEPTH, w / 2 - half, h), (w / 2 + half, h - CARD_TONGUE_DEPTH, w, h)]
    # Parts stay off the whole tongue: it goes into the socket body (7.8 mm past the host edge), so
    # only tracks may run there. Vias and pour stay out of the 4 mm finger zone only.
    return {"size": (w, h), "layers": 2, "fixed": fixed, "finger_zone": 4.0, "parts_clear_bottom": CARD_TONGUE_DEPTH,
            "near": near, "cuts": cuts}


def dbg_spec() -> dict:
    """TL-DBG-001: the slot socket straddles the top edge (card plugs in beside the adapter, coplanar,
    as on the main board); USB-C on the bottom edge; meter headers either side of the socket."""
    w, h = 40.0, 36.0
    fixed = {
        "J2": (w / 2, 1.0, 180), "J1": (w / 2, h - 4.0, 0),
        "JP1": (4.0, 8.0, 0), "JP2": (w - 4.0, 8.0, 0),
        "U3": (w / 2, 17.0, 90), "U2": (8.0, h - 8.0, 0), "U1": (w / 2 + 8.0, h - 9.0, 0),
        "U4": (8.0, 12.0, 0), "U5": (w - 8.0, 12.0, 0), "U6": (w / 2 + 7.0, 9.0, 0), "D2": (w / 2 - 7.0, 9.0, 0),
    }
    near = {"C1": "U2", "C2": "U2", "C3": "U3", "C4": "U3", "R3": "U3", "C5": "U4", "R4": "U4", "C6": "U5",
            "R5": "U5", "R6": "U5", "C7": "U6", "R1": "J1", "R2": "J1"}
    return {"size": (w, h), "layers": 2, "fixed": fixed, "near": near, "gap": 0.8,
            "pinned": {"J1", "J2", "JP1", "JP2"}, "optimize": True, "slot_sockets": ["J2"]}


SPECS = {"TL-SN-001": sn_spec()}
for b in ("TL-MOD-001", "TL-MOD-002", "TL-MOD-003", "TL-MOD-004", "TL-TEST-001"):
    SPECS[b] = card_spec(b)
SPECS["TL-DBG-001"] = dbg_spec()


DRU_SN = """(version 1)
# Generated by hardware/scripts/gen_pcbs.py.
(rule "edge-mount slot sockets straddle the board edge by design"
  (condition "A.memberOfFootprint('J4') || A.memberOfFootprint('J5') || A.memberOfFootprint('J6') || A.memberOfFootprint('J7') || B.memberOfFootprint('J4') || B.memberOfFootprint('J5') || B.memberOfFootprint('J6') || B.memberOfFootprint('J7')")
  (constraint edge_clearance (min 0mm)))
"""

DRU_DBG = """(version 1)
# Generated by hardware/scripts/gen_pcbs.py.
(rule "edge-mount slot socket straddles the board edge by design"
  (condition "A.memberOfFootprint('J2') || B.memberOfFootprint('J2')")
  (constraint edge_clearance (min 0mm)))
"""

DRU_CARD = """(version 1)
# Generated by hardware/scripts/gen_pcbs.py.
(rule "card-edge fingers run to the board edge by design"
  (condition "A.memberOfFootprint('J1') || B.memberOfFootprint('J1')")
  (constraint edge_clearance (min 0mm)))
"""


# ---- helpers ----------------------------------------------------------------------------
def load_fp(fpid: str) -> pcbnew.FOOTPRINT:
    lib, name = fpid.split(":", 1)
    path = TALOS_FP if lib == "talos" else STOCK_FP / f"{lib}.pretty"
    fp = pcbnew.FootprintLoad(str(path), name)
    if fp is None:
        raise SystemExit(f"cannot load footprint {fpid}")
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


def bbox(fp: pcbnew.FOOTPRINT) -> tuple[float, float, float, float]:
    """Courtyard extents (both sides), so keepout zones inside a footprint don't block placement."""
    boxes = []
    for layer in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
        poly = fp.GetCourtyard(layer)
        if poly.OutlineCount():
            boxes.append(poly.BBox())
    if not boxes:
        boxes = [fp.GetBoundingBox(False)]
    b = boxes[0]
    for o in boxes[1:]:
        b.Merge(o)
    return (pcbnew.ToMM(b.GetX()) - ORIGIN[0], pcbnew.ToMM(b.GetY()) - ORIGIN[1],
            pcbnew.ToMM(b.GetRight()) - ORIGIN[0], pcbnew.ToMM(b.GetBottom()) - ORIGIN[1])


def overlaps(a, b, gap=0.6) -> bool:
    return not (a[2] + gap <= b[0] or b[2] + gap <= a[0] or a[3] + gap <= b[1] or b[3] + gap <= a[1])


def anchor_of(ref: str, part: dict, parts: dict, fixed: dict) -> str | None:
    nets = {n for n in part["pins"].values() if n and n not in ("GND",)}
    best, score = None, 0
    for other, op in parts.items():
        if other == ref or other not in fixed:
            continue
        s = len(nets & {n for n in op["pins"].values() if n})
        if s > score:
            best, score = other, s
    return best


def _poly(points) -> pcbnew.SHAPE_POLY_SET:
    ps = pcbnew.SHAPE_POLY_SET()
    ps.NewOutline()
    for x, y in points:
        ps.Append(mm(ORIGIN[0] + x), mm(ORIGIN[1] + y))
    return ps


def slot_notch(fp: pcbnew.FOOTPRINT) -> list[tuple[float, float]]:
    """The HSEC8 locating slot in board mm, mapped from the footprint frame through two of its pads
    (so the rotation convention can't be got wrong)."""
    pads = {p.GetNumber(): p for p in fp.Pads() if p.IsOnLayer(pcbnew.F_Cu)}
    ly = HSEC8_EDGE_Y - 1.8
    l1, l19 = (HSEC8_PIN1_X, ly), (HSEC8_PIN1_X - 9 * HSEC8_PITCH, ly)
    b1, b19 = ((pcbnew.ToMM(pads[n].GetPosition().x) - ORIGIN[0], pcbnew.ToMM(pads[n].GetPosition().y) - ORIGIN[1])
               for n in ("1", "19"))
    ang = math.atan2(b19[1] - b1[1], b19[0] - b1[0]) - math.atan2(l19[1] - l1[1], l19[0] - l1[0])
    c, s_ = math.cos(ang), math.sin(ang)

    def to_board(x, y):
        dx, dy = x - l1[0], y - l1[1]
        return (b1[0] + c * dx - s_ * dy, b1[1] + s_ * dx + c * dy)

    sx, r, depth = HSEC8_SLOT["x"], HSEC8_SLOT["width"] / 2, HSEC8_SLOT["depth"]
    e = HSEC8_EDGE_Y
    cy = e - depth + r  # centre of the full-radius end
    local = [(sx + r, e + 1.0), (sx + r, cy)]
    local += [(sx + r * math.cos(-math.pi * k / 12), cy + r * math.sin(-math.pi * k / 12)) for k in range(1, 12)]
    local += [(sx - r, cy), (sx - r, e + 1.0)]
    return [to_board(x, y) for x, y in local]


def add_outline(board, w, h, cuts=(), notches=()) -> None:
    """Board outline: the w x h rectangle minus rectangular cuts (card tongue) and socket slots."""
    outline = _poly([(0, 0), (w, 0), (w, h), (0, h)])
    for x0, y0, x1, y1 in cuts:
        outline.BooleanSubtract(_poly([(x0, y0), (x1, y0), (x1, y1), (x0, y1)]))
    for n in notches:
        outline.BooleanSubtract(_poly(n))
    if outline.OutlineCount() != 1 or outline.HoleCount(0):
        raise SystemExit("board outline is not a single closed contour")
    chain = outline.Outline(0)
    pts = [chain.CPoint(i) for i in range(chain.PointCount())]
    for i, a in enumerate(pts):
        seg = pcbnew.PCB_SHAPE(board)
        seg.SetShape(pcbnew.SHAPE_T_SEGMENT)
        seg.SetStart(a)
        seg.SetEnd(pts[(i + 1) % len(pts)])
        seg.SetLayer(pcbnew.Edge_Cuts)
        seg.SetWidth(mm(0.1))
        board.Add(seg)


def add_zone(board, net, layer, rect, priority=0) -> None:
    z = pcbnew.ZONE(board)
    z.SetLayer(layer)
    z.SetNet(net)
    z.SetAssignedPriority(priority)
    z.SetLocalClearance(mm(0.25))
    z.SetMinThickness(mm(0.2))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)  # reflow-only assembly
    z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)  # no floating copper
    ol = z.Outline()
    ol.NewOutline()
    x0, y0, x1, y1 = rect
    for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
        ol.Append(mm(ORIGIN[0] + x), mm(ORIGIN[1] + y))
    board.Add(z)


def add_rule_area(board, rect, layers, name, allow_tracks=False) -> None:
    z = pcbnew.ZONE(board)
    z.SetIsRuleArea(True)
    z.SetDoNotAllowTracks(not allow_tracks)
    z.SetDoNotAllowVias(True)
    z.SetDoNotAllowZoneFills(True)
    z.SetDoNotAllowPads(False)
    z.SetDoNotAllowFootprints(False)
    z.SetZoneName(name)
    ls = pcbnew.LSET()
    for layer in layers:
        ls.AddLayer(layer)
    z.SetLayerSet(ls)
    ol = z.Outline()
    ol.NewOutline()
    x0, y0, x1, y1 = rect
    for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
        ol.Append(mm(ORIGIN[0] + x), mm(ORIGIN[1] + y))
    board.Add(z)


def flip_two_pad_parts(fps: dict, pinned: set) -> None:
    """Turn two-pad parts 180 degrees where that brings each pad nearer its own net.

    Wirelength alone can't tell the two orientations apart: an inductor between two switch pins
    costs the same either way, but one way round its nets have to cross (TPS63900 LX1/LX2 to L1).
    Only parts whose courtyard is symmetric about the origin are flipped (resistors, caps), so the
    flip never breaks the legal placement. A pin header's origin is pin 1: turning it moves its body.
    """
    def cost(fp) -> float:
        total = 0.0
        for pad in fp.Pads():
            others = [q.GetPosition() for f in fps.values() if f is not fp for q in f.Pads()
                      if q.GetNetCode() == pad.GetNetCode() and pad.GetNetCode() > 0]
            if others:
                p = pad.GetPosition()
                total += min(math.hypot(p.x - o.x, p.y - o.y) for o in others)
        return total

    for ref, fp in sorted(fps.items()):
        if ref in pinned or len(fp.Pads()) != 2:
            continue
        before, box = cost(fp), bbox(fp)
        fp.SetOrientationDegrees(fp.GetOrientationDegrees() + 180)
        moved = any(abs(a - b) > 0.01 for a, b in zip(box, bbox(fp)))
        if moved or cost(fp) >= before - 1e-6:
            fp.SetOrientationDegrees(fp.GetOrientationDegrees() - 180)


def optimise_placement(spec, parts, fps, w, h, margin, keep_top, keep_bottom) -> None:
    """Refine the rule-based start with the annealing placer; pinned parts stay where the spec put them."""
    pinned = spec.get("pinned", set())
    probs = {}
    for ref, fp in fps.items():
        x0, y0 = (pcbnew.ToMM(v) - o for v, o in zip((fp.GetPosition().x, fp.GetPosition().y), ORIGIN))
        rot0 = int(round(fp.GetOrientationDegrees())) % 360
        shapes = {}
        for rot in place_opt.ROTS:
            fp.SetOrientationDegrees(rot)
            fp.SetPosition(pt(0, 0))
            pads = []
            for pad in fp.Pads():
                num = PAD_ALIASES.get(pad.GetNumber(), pad.GetNumber())
                net = parts[ref]["pins"].get(num)
                p = pad.GetPosition()
                pads.append((pcbnew.ToMM(p.x) - ORIGIN[0], pcbnew.ToMM(p.y) - ORIGIN[1], net or ""))
            shapes[rot] = (bbox(fp), pads)
        fp.SetOrientationDegrees(rot0)
        fp.SetPosition(pt(x0, y0))
        is_ic = ref.startswith("U")
        probs[ref] = place_opt.Part(ref=ref, movable=ref not in pinned, shapes=shapes, x=x0, y=y0,
                                    rot=rot0 if rot0 in place_opt.ROTS else 0,
                                    halo=0.8 if is_ic else 0.35, anchor=spec.get("near", {}).get(ref))
    area = (margin, keep_top + margin, w - margin, h - keep_bottom - margin)
    prob = place_opt.Problem(parts=probs, area=area, plane_nets={"GND"})
    sol = place_opt.optimize(prob, seed=spec.get("seed", 1), iterations=spec.get("iterations", 150_000))
    for ref, (x, y, rot) in sol.items():
        fps[ref].SetOrientationDegrees(rot)
        fps[ref].SetPosition(pt(x, y))


FIDUCIAL = "Fiducial:Fiducial_1mm_Mask2mm"
REVISION = "A"


def silk_text(board, text, x, y, size=1.0, layer=None) -> None:
    t = pcbnew.PCB_TEXT(board)
    t.SetText(text)
    t.SetLayer(layer if layer is not None else pcbnew.F_SilkS)
    t.SetPosition(pt(x, y))
    t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
    t.SetTextThickness(mm(max(0.1, size * 0.15)))
    board.Add(t)


def silk_box(board, x0, y0, x1, y1, layer=None) -> None:
    r = pcbnew.PCB_SHAPE(board)
    r.SetShape(pcbnew.SHAPE_T_RECT)
    r.SetStart(pt(x0, y0))
    r.SetEnd(pt(x1, y1))
    r.SetLayer(layer if layer is not None else pcbnew.F_SilkS)
    r.SetWidth(mm(0.12))
    board.Add(r)


def add_marking(board, name, spec, fps, w, h, copper) -> None:
    """Board name, revision and a serial box on the bottom silk (bench §8: all boards); fiducials on
    the assembled boards big enough to carry them; card-specific marks."""
    cards = spec.get("finger_zone")
    size = 0.7 if cards else 1.0
    y = 3.0 if cards else h - (12.0 if name == "TL-SN-001" else 8.0)
    x = w / 2
    silk_text(board, f"{name} rev {REVISION}", x, y, size, pcbnew.B_SilkS)
    silk_text(board, "S/N", x - (2.4 if cards else 4.5), y + size * 1.8, size * 0.8, pcbnew.B_SilkS)
    bw, bh = (5.0, 1.6) if cards else (9.0, 2.4)
    silk_box(board, x - bw / 2 + 1.0, y + size * 1.8 - bh / 2, x + bw / 2 + 1.0, y + size * 1.8 + bh / 2,
             pcbnew.B_SilkS)
    if name == "TL-SN-001":  # smaller boards rely on the JLCPCB panel-rail fiducials
        # Board-only footprints (no symbol): three corners, clear of the edge ring and the antenna strip.
        top = spec.get("keepout_top", 0.0) + 3.0
        for i, (fx, fy) in enumerate(((3.0, top), (w - 3.0, h - 3.0), (3.0, h - 3.0))):
            fp = load_fp(FIDUCIAL)
            fp.SetReference(f"FID{i + 1}")
            fp.SetValue("fiducial")
            fp.SetBoardOnly(True)
            fp.SetExcludedFromBOM(True)
            fp.SetExcludedFromPosFiles(True)
            taken = [bbox(f) for f in fps.values()]
            for r in range(0, 40):  # walk in from the corner until the fiducial's courtyard is clear
                cx = fx + (r * 0.5 if fx < w / 2 else -r * 0.5)
                cy = fy + (r * 0.5 if fy < h / 2 else -r * 0.5)
                fp.SetPosition(pt(cx, cy))
                if not any(overlaps(bbox(fp), t, 0.3) for t in taken):
                    break
            else:
                raise SystemExit(f"{name}: no room for fiducial {i + 1}")
            board.Add(fp)
    if name == "TL-MOD-002":  # EKMB pin legend beside the sensor (bench §8)
        u = fps["U2"]
        ux, uy = pcbnew.ToMM(u.GetPosition().x) - ORIGIN[0], pcbnew.ToMM(u.GetPosition().y) - ORIGIN[1]
        silk_text(board, "V O G", ux, uy + 6.2, 0.8)
    if name == "TL-MOD-004":  # pull every pour back ~1 mm around the SHT40 (DS §5.3): heat from copper
        u = fps["U2"]
        x0, y0, x1, y1 = bbox(u)
        add_rule_area(board, (x0 - 1.0, y0 - 1.0, x1 + 1.0, y1 + 1.0), copper,
                      "SHT40 thermal keepout: no pour", allow_tracks=True)


def build(board_name: str) -> dict:
    spec = SPECS[board_name]
    intended = json.load(open(KICAD / board_name / "intended-netlist.json"))
    parts = intended["parts"]
    w, h = spec["size"]
    board = pcbnew.BOARD()
    board.SetCopperLayerCount(spec["layers"])
    ds = board.GetDesignSettings()
    ds.SetCopperLayerCount(spec["layers"])
    ds.m_ViasMinSize = mm(0.5)
    ds.m_MinThroughDrill = mm(0.3)
    ds.SetBoardThickness(mm(1.6))
    if spec["layers"] == 4:  # In1 = GND plane, In2 = V3V3_CORE plane; signals route on F.Cu and B.Cu
        board.SetLayerType(pcbnew.In1_Cu, pcbnew.LT_POWER)
        board.SetLayerType(pcbnew.In2_Cu, pcbnew.LT_POWER)

    nets = {}
    for p in parts.values():
        for n in p["pins"].values():
            if n and n not in nets:
                ni = pcbnew.NETINFO_ITEM(board, f"/{n}")
                board.Add(ni)
                nets[n] = ni

    root = uid(board_name, "root")
    placed: dict[str, tuple] = {}
    fps: dict[str, pcbnew.FOOTPRINT] = {}
    for ref, p in parts.items():
        fp = load_fp(p["footprint"])
        fp.SetReference(ref)
        fp.SetValue(p["value"])
        fp.SetPath(pcbnew.KIID_PATH(f"/{root}/{uid(board_name, ref)}"))
        if p["note"]:
            field = pcbnew.PCB_FIELD(fp, fp.GetNextFieldOrdinal(), "Talos_Note")
            field.SetText(p["note"])
            field.SetVisible(False)
            field.SetLayer(pcbnew.F_Fab)
            fp.Add(field)
        fp.SetField("Datasheet", p["datasheet"])
        if p.get("dnp"):
            fp.SetDNP(True)  # placed and routed, not assembled (bench option)
        for pad in fp.Pads():
            num = PAD_ALIASES.get(pad.GetNumber(), pad.GetNumber())
            if num not in p["pins"]:
                continue  # mechanical pads (mounting tabs) stay unconnected
            net = p["pins"][num]
            if net is None:  # mirror the schematic's name for intentionally unconnected pins
                net = f"unconnected-({ref}-{p['pin_names'][num]}-Pad{pad.GetNumber()})"
                if net not in nets:
                    nets[net] = pcbnew.NETINFO_ITEM(board, net)
                    board.Add(nets[net])
                pad.SetNet(nets[net])
                continue
            pad.SetNet(nets[net])
        board.Add(fp)
        fps[ref] = fp

    # fixed placements
    for ref, (x, y, rot) in spec["fixed"].items():
        fp = fps[ref]
        fp.SetPosition(pt(x, y))
        fp.SetOrientationDegrees(rot)
        placed[ref] = bbox(fp)

    for i, c in enumerate(spec.get("cuts", ())):
        placed[f"_cut{i}"] = c  # nothing may be placed where the outline is cut away
    margin = 1.0
    keep_top = spec.get("keepout_top", 0.0)
    keep_bottom = spec.get("finger_zone", 0.0)
    parts_bottom = max(keep_bottom, spec.get("parts_clear_bottom", 0.0))

    def free(b) -> bool:
        if b[0] < margin or b[2] > w - margin or b[1] < keep_top + margin or b[3] > h - parts_bottom - margin:
            return False
        return not any(overlaps(b, o, spec.get("gap", 0.6)) for o in placed.values())

    # everything else: spiral out from the anchor IC
    hinted = spec.get("near", {})
    rest = sorted((r for r in parts if r not in placed), key=lambda r: (r not in hinted, r[0] != "C", r))
    for ref in rest:
        fp = fps[ref]
        anchor = spec.get("near", {}).get(ref) or anchor_of(ref, parts[ref], parts, placed)
        if anchor not in placed:
            anchor = anchor_of(ref, parts[ref], parts, placed)
        if anchor:
            a = placed[anchor]
            cx, cy = (a[0] + a[2]) / 2, (a[1] + a[3]) / 2
        else:
            cx, cy = w / 2, h / 2
        done = False
        for ring in range(0, 400):
            r = 1.0 + ring * 0.5
            steps = max(8, int(2 * math.pi * r / 1.0))
            for k in range(steps):
                ang = 2 * math.pi * k / steps
                for rot in (0, 90):
                    fp.SetOrientationDegrees(rot)
                    fp.SetPosition(pt(cx + r * math.cos(ang), cy + r * math.sin(ang)))
                    b = bbox(fp)
                    if free(b):
                        placed[ref] = b
                        done = True
                        break
                if done:
                    break
            if done:
                break
        if not done:
            raise SystemExit(f"{board_name}: no room for {ref}")

    if spec.get("optimize"):
        optimise_placement(spec, parts, fps, w, h, margin, keep_top, keep_bottom)
        flip_two_pad_parts(fps, spec.get("pinned", set()))

    if spec.get("parts_clear_bottom"):
        intruders = [r for r, b in placed.items() if not r.startswith("_cut") and r != "J1"
                     and b[3] > h - spec["parts_clear_bottom"]]
        if intruders:
            raise SystemExit(f"{board_name}: parts on the card tongue (inside the socket): {intruders}")

    notches = [slot_notch(fps[r]) for r in spec.get("slot_sockets", ())]
    add_outline(board, w, h, spec.get("cuts", ()), notches)
    ds.SetAuxOrigin(pt(0, h))  # fab/pick-and-place origin at the board's lower-left corner
    gnd = nets["GND"]
    copper = [pcbnew.F_Cu, pcbnew.B_Cu] if spec["layers"] == 2 else [pcbnew.F_Cu, pcbnew.In1_Cu, pcbnew.In2_Cu, pcbnew.B_Cu]
    pour = (0.3, 0.3 + keep_top, w - 0.3, h - 0.3 - keep_bottom)
    for layer in copper:
        net = nets[spec["plane_nets"][layer]] if layer in spec.get("plane_nets", {}) else gnd
        add_zone(board, net, layer, pour)
    # 0.35 mm track/via-free ring inside the outline (pads still allowed: edge connectors sit there)
    ring = 0.35
    for rect in ((0, 0, w, ring), (0, h - ring, w, h), (0, 0, ring, h), (w - ring, 0, w, h)):
        add_rule_area(board, rect, copper, "edge clearance ring")
    for x0, y0, x1, y1 in spec.get("cuts", ()):
        add_rule_area(board, (x0 - ring, y0 - ring, x1 + ring, y1), copper, "edge clearance ring (tongue)")
    for n in notches:
        xs, ys = [p[0] for p in n], [p[1] for p in n]
        add_rule_area(board, (min(xs) - ring, min(ys) - ring, max(xs) + ring, max(ys) + ring), copper,
                      "edge clearance ring (socket slot)")
    if keep_top:
        add_rule_area(board, (0, 0, w, keep_top), copper, "antenna keepout (ESP32-H2-MINI-1)")
    if keep_bottom:
        add_rule_area(board, (0, h - keep_bottom, w, h), [pcbnew.F_Cu, pcbnew.B_Cu],
                      "card-edge fingers: tracks only, no pour or vias", allow_tracks=True)

    add_marking(board, board_name, spec, fps, w, h, copper)

    (KICAD / board_name / f"{board_name}.kicad_dru").write_text(
        {"TL-SN-001": DRU_SN, "TL-DBG-001": DRU_DBG}.get(board_name, DRU_CARD))
    out = KICAD / board_name / f"{board_name}.kicad_pcb"
    board.Save(str(out))
    return {"board": board_name, "footprints": len(fps), "nets": len(nets), "size": f"{w}x{h} mm",
            "layers": spec["layers"]}


def main() -> int:
    for b in sys.argv[1:] or list(SPECS):
        info = build(b)
        print(f"{info['board']}: {info['footprints']} footprints, {info['nets']} nets, {info['size']}, "
              f"{info['layers']} layers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
