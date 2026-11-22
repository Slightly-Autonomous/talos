"""Housing marks on TL-SN-002 (owner-approved 2026-11-22: B-04, B-06, H-05, H-06) and the board
geometry the printed housing is built from.

    python3 hardware/scripts/housing_marks.py [TL-SN-002]     # pcbnew (KiCad's Python)

Idempotent: it removes its own drawings, then redraws them from the placed footprints. It runs after
routing (route_pcbs.py calls it); it never moves a part or a track.

- User.Eco1 "HOUSING": everything the enclosure has to respect.
  - Heat-set insert bosses at H1-H4.
  - The antenna zone: no metal or screws within 15 mm of the module's antenna end (H-03).
  - The lid and battery-door spring-finger targets over SW4 and SW5 (HD-049).
  - The battery-door opening over the AA holder.
  - The PIR lens window.
  - The thin wall at the Hall sensor (<= 1.5 mm, facing the door magnet).
  - The SHT40 tongue vent.
  - The USB-C opening.
  - One light-pipe position per lamp (H-06).
- User.Eco2 "NO COAT": the conformal-coating keep-out map (B-06). Covers connectors, the Tag-Connect
  pads, the SHT40 and its tongue, the PIR window, every button and tamper switch, the battery contacts,
  test pads and the solder jumper.
- hardware/mechanical/housing/TL-SN-002.board.json: the same geometry as numbers (mm, board origin at
  the top-left corner, y down) for hardware/mechanical/housing/housing.py.
"""

from __future__ import annotations

import json
import os
import pathlib
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from boards import kicad_dir  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
ORIGIN = (100.0, 100.0)  # gen_pcbs.ORIGIN
HOUSING = pcbnew.Eco1_User
NO_COAT = pcbnew.Eco2_User
TAG = "housing_marks"  # every drawing this script owns carries it in its text or is grouped
LAMP_ROLE = {"D8": "PWR", "D1": "STAT", "D9": "LINK", "D10": "MAG", "D11": "VIB", "D12": "PIR",
             "D13": "ENV", "D14": "TAMP", "D15": "BATT", "D16": "FLT"}
NO_COAT_PREFIX = ("J", "SW", "TP", "JP", "BT")
NO_COAT_REFS = ("U9", "U10", "C17")


def mm(v: float) -> int:
    return pcbnew.FromMM(v)


def to_mm(p) -> tuple[float, float]:
    return round(pcbnew.ToMM(p.x) - ORIGIN[0], 3), round(pcbnew.ToMM(p.y) - ORIGIN[1], 3)


def box_mm(b) -> list[float]:
    return [round(pcbnew.ToMM(b.GetX()) - ORIGIN[0], 3), round(pcbnew.ToMM(b.GetY()) - ORIGIN[1], 3),
            round(pcbnew.ToMM(b.GetRight()) - ORIGIN[0], 3), round(pcbnew.ToMM(b.GetBottom()) - ORIGIN[1], 3)]


def courtyard(fp) -> list[float]:
    c = fp.GetCourtyard(pcbnew.F_CrtYd)
    return box_mm(c.BBox() if c.OutlineCount() else fp.GetBoundingBox(False, False))


class Drawer:
    def __init__(self, board):
        self.board = board
        self.group = pcbnew.PCB_GROUP(board)
        self.group.SetName(TAG)
        board.Add(self.group)

    def _add(self, item):
        self.board.Add(item)
        self.group.AddItem(item)

    def rect(self, layer, x0, y0, x1, y1, width=0.1):
        s = pcbnew.PCB_SHAPE(self.board, pcbnew.SHAPE_T_RECT)
        s.SetStart(pcbnew.VECTOR2I(mm(ORIGIN[0] + x0), mm(ORIGIN[1] + y0)))
        s.SetEnd(pcbnew.VECTOR2I(mm(ORIGIN[0] + x1), mm(ORIGIN[1] + y1)))
        s.SetLayer(layer)
        s.SetWidth(mm(width))
        self._add(s)

    def circle(self, layer, x, y, r, width=0.1):
        s = pcbnew.PCB_SHAPE(self.board, pcbnew.SHAPE_T_CIRCLE)
        s.SetCenter(pcbnew.VECTOR2I(mm(ORIGIN[0] + x), mm(ORIGIN[1] + y)))
        s.SetEnd(pcbnew.VECTOR2I(mm(ORIGIN[0] + x + r), mm(ORIGIN[1] + y)))
        s.SetLayer(layer)
        s.SetWidth(mm(width))
        self._add(s)

    def text(self, layer, x, y, txt, size=0.8):
        t = pcbnew.PCB_TEXT(self.board)
        t.SetText(txt)
        t.SetPosition(pcbnew.VECTOR2I(mm(ORIGIN[0] + x), mm(ORIGIN[1] + y)))
        t.SetLayer(layer)
        t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
        t.SetTextThickness(mm(size * 0.12))
        self._add(t)


def clear_old(board) -> None:
    for g in list(board.Groups()):
        if g.GetName() == TAG:
            for item in list(g.GetItems()):
                board.Remove(item)
            board.Remove(g)


def run(name: str) -> dict:
    pcb_path = kicad_dir(name) / f"{name}.kicad_pcb"
    board = pcbnew.LoadBoard(str(pcb_path))
    clear_old(board)
    fps = {f.GetReference(): f for f in board.Footprints()}
    edge = box_mm(board.GetBoardEdgesBoundingBox())
    w, h = edge[2] - edge[0], edge[3] - edge[1]
    d = Drawer(board)
    geo: dict = {"board": name, "size_mm": [round(w, 3), round(h, 3)], "thickness_mm": 1.6,
                 "origin": "top-left corner of the board outline, y down, top side up",
                 "generated_by": "hardware/scripts/housing_marks.py"}

    # Mounting holes: M2.5 heat-set insert bosses (H-01), 5.0 mm boss, 3.6 mm insert hole.
    holes = []
    for ref in sorted(r for r in fps if r.startswith("H") and fps[r].IsBoardOnly()):
        x, y = to_mm(fps[ref].GetPosition())
        holes.append({"ref": ref, "x": x, "y": y, "drill_mm": 2.7})
        d.circle(HOUSING, x, y, 2.5)
        d.text(HOUSING, x, y + 3.3, "BOSS", 0.6)
    geo["holes"] = holes

    # Antenna zone (H-03): from the module's antenna end, 15 mm free of metal; the module's
    # copper keep-out strip is the board's top band.
    u1 = courtyard(fps["U1"])
    ant = [max(0.0, u1[0] - 2.0), 0.0, min(w, u1[2] + 2.0), 15.0]
    d.rect(HOUSING, *ant)
    d.text(HOUSING, (ant[0] + ant[2]) / 2, 7.5, "ANTENNA: NO METAL / SCREWS 15 mm", 0.7)
    geo["antenna_zone"] = ant

    # Tamper switch targets (HD-049): the finger lands on the 5.1 mm actuator; travel 0.25 mm,
    # the printed spring takes up the tolerance. Actuator top is 1.5 mm above the board.
    switches = {}
    for ref, what in (("SW4", "LID FINGER"), ("SW5", "DOOR FINGER")):
        x, y = to_mm(fps[ref].GetPosition())
        switches[ref] = {"x": x, "y": y, "actuator_top_mm": 1.5, "travel_mm": 0.25}
        d.circle(HOUSING, x, y, 1.6)
        d.text(HOUSING, x, y - 3.0, what, 0.6)
    geo["tamper_switches"] = switches

    # Battery door opening over the holder, 1.5 mm margin; the door also covers SW5.
    bt = courtyard(fps["BT1"])
    sw5 = courtyard(fps["SW5"])
    door = [bt[0] - 1.5, bt[1] - 1.5, max(bt[2], sw5[2]) + 1.0, min(h, bt[3] + 1.5)]
    d.rect(HOUSING, *door)
    d.text(HOUSING, (door[0] + door[2]) / 2, door[1] + 2.0, "BATTERY DOOR (TOOL-FREE)", 0.8)
    geo["battery_holder"] = {"courtyard": bt, "height_mm": 16.0, "cells": "2 x AA"}
    geo["battery_door_opening"] = door

    # PIR lens window: the TO-5 can is 9.1 mm, 4.5 mm tall; the Fresnel lens sits over it.
    px, py = to_mm(fps["U10"].GetPosition())
    d.circle(HOUSING, px, py, 6.5)
    d.text(HOUSING, px, py + 7.5, "PIR LENS", 0.7)
    geo["pir"] = {"x": px, "y": py, "can_diameter_mm": 9.1, "can_height_mm": 4.5, "window_diameter_mm": 13.0}

    # Hall sensor at the right edge: the wall there faces the door magnet, <= 1.5 mm thick.
    hx, hy = to_mm(fps["U6"].GetPosition())
    d.rect(HOUSING, hx - 2.0, hy - 4.0, w, hy + 4.0)
    d.text(HOUSING, hx - 5.0, hy, "THIN WALL 1.5", 0.6)
    geo["hall"] = {"x": hx, "y": hy, "edge": "right", "max_wall_mm": 1.5}

    # SHT40 tongue: vented compartment, ePTFE outdoors (H-09), never coated.
    sx, sy = to_mm(fps["U9"].GetPosition())
    import gen_pcbs as g
    tx0 = min(c[0] for c in g.SN2_TONGUE_SLOTS)
    ty0, ty1 = g.SN2_TONGUE_SLOTS[0][1], g.SN2_TONGUE_SLOTS[1][3]
    d.rect(HOUSING, tx0, ty0, w, ty1)
    d.text(HOUSING, tx0 + 3.0, ty1 + 1.2, "VENT", 0.6)
    geo["sht40"] = {"x": sx, "y": sy, "tongue": [tx0, ty0, w, ty1]}

    # USB-C opening on the bottom edge (stays reachable, owner 2026-11-22; TPU plug outdoors).
    j1 = courtyard(fps["J1"])
    d.rect(HOUSING, j1[0] - 0.5, j1[3] - 4.0, j1[2] + 0.5, h)
    d.text(HOUSING, (j1[0] + j1[2]) / 2, j1[3] - 5.0, "USB-C OPENING", 0.6)
    geo["usb_c"] = {"x": round((j1[0] + j1[2]) / 2, 3), "edge": "bottom", "width_mm": round(j1[2] - j1[0], 3),
                    "opening_mm": [9.5, 3.8]}

    # Light pipes (H-06): one 1.8 mm pipe per lamp, straight up through the lid.
    lamps = []
    for ref, role in LAMP_ROLE.items():
        if ref not in fps:
            continue
        x, y = to_mm(fps[ref].GetPosition())
        lamps.append({"ref": ref, "role": role, "x": x, "y": y})
        d.circle(HOUSING, x, y, 0.9)
    geo["lamps"] = lamps
    geo["buttons"] = {r: dict(zip(("x", "y"), to_mm(fps[r].GetPosition()))) for r in ("SW1", "SW2", "SW3")}
    # Tall parts the lid must clear (body tops, mm above the board; REF datasheet values).
    geo["tall_parts_mm"] = {"BT1": 16.0, "U10": 4.5 + 1.0, "J1": 3.3, "U1": 3.2}

    # Conformal-coat keep-outs (B-06).
    coat = []
    for ref, fp in sorted(fps.items()):
        if ref.startswith(NO_COAT_PREFIX) or ref in NO_COAT_REFS:
            if fp.IsBoardOnly() and not ref.startswith("TP"):
                continue
            b = courtyard(fp)
            d.rect(NO_COAT, b[0] - 0.5, b[1] - 0.5, b[2] + 0.5, b[3] + 0.5)
            coat.append(ref)
    d.rect(NO_COAT, tx0, ty0, w, ty1)  # the whole SHT40 tongue
    d.text(NO_COAT, w / 2, h - 1.0, "NO CONFORMAL COAT INSIDE OUTLINES", 0.8)
    geo["no_coat"] = coat

    board.Save(str(pcb_path))
    out = ROOT / "hardware" / "mechanical" / "housing" / f"{name}.board.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(geo, indent=1) + "\n")
    return {"board": name, "lamps": len(lamps), "no_coat": len(coat), "json": str(out.relative_to(ROOT))}


def main() -> int:
    for name in sys.argv[1:] or ["TL-SN-002"]:
        print(run(name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
