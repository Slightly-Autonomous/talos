"""Layout invariants on the committed boards: decisions that a future regeneration must not silently undo.

    python3 hardware/scripts/layout_rules.py          # needs pcbnew (KiCad's Python, or the kicad/kicad image)

DRC checks copper rules; this checks design intent that DRC can't see:
- HD-036/HD-037: the regulator and charger templates keep their parts next to the right pins.
- HD-041: thermal pads carry at least two vias.
- HD-039: the project hole-to-hole rule stays at 0.5 mm.
- HD-040: card finger copper stops at least 0.6 mm short of the card edge.
- HD-038: the Power/Rail length left narrower than its netclass stays within the board's budget
  (layout-status.json `narrow_power_mm_max`).
- M-01/M-02: mounting holes present, unplated, right size. M-04: the JLCPCB order-number text is on the
  bottom silk. M-06: the lamp strip sits on the bottom edge. S-01/S-03: every label in silk_pass.LABELS
  is printed.
CI runs it in the KiCad image after check_kicad.py; it never routes or places anything.
"""

from __future__ import annotations

import json
import math
import os
import pathlib
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from silk_pass import LABELS  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
KICAD = ROOT / "hardware" / "kicad"
from boards import ACTIVE as BOARDS  # noqa: E402
CARDS = {"TL-MOD-001", "TL-MOD-002", "TL-MOD-003", "TL-MOD-004", "TL-TEST-001"}
mm = pcbnew.ToMM

# (part, its pad on `net`, anchor, anchor pin numbers on that net, max centre distance mm)
TEMPLATE_RULES = [
    ("C35", "CHG_IN", "U4", ("10",), 2.6),  # HD-037: BQ25186 IN local cap
    ("C9", "SYS_PATH", "U4", ("1",), 3.5),
    ("L1", "REG_LX1", "U5", ("9",), 3.5),  # HD-036: inductor against the LX pins
    ("L1", "REG_LX2", "U5", ("7",), 3.5),
    ("C11", "SYS_REG", "U5", ("1", "10"), 3.5),
    ("C12", "V3V3_REG", "U5", ("6",), 3.5),
]
HOLES = {"TL-SN-001": 2.7, "TL-SN-002": 2.7, "TL-DBG-001": 3.2}
LAMP_STRIP = ("D1", "D8", "D9", "D10", "D11", "D12")
# TL-SN-002 (ADR 0019): the same TPS63900 template, lamps on the left edge, SHT40 on the slotted tongue,
# the Hall sensor on the right edge facing the door magnet.
SN2_TEMPLATE = [r for r in TEMPLATE_RULES if r[2] == "U5"]
SN2_LAMPS = ("D8", "D1", "D9", "D10", "D11", "D12", "D13", "D14", "D15", "D16")


def _pad(fp, net=None, number=None):
    for p in fp.Pads():
        if (net is None or p.GetNetname().lstrip("/") == net) and (number is None or p.GetNumber() == number):
            return p
    return None


def _dist(a, b) -> float:
    d = a.GetPosition() - b.GetPosition()
    return math.hypot(mm(d.x), mm(d.y))


def check(name: str, status: dict) -> list[str]:
    pcb = KICAD / name / f"{name}.kicad_pcb"
    board = pcbnew.LoadBoard(str(pcb))
    fps = {f.GetReference(): f for f in board.Footprints()}
    edge = board.GetBoardEdgesBoundingBox()
    problems: list[str] = []
    # A checkpoint board may waive named rules, each with a reason in layout-status.json (never silently).
    waived = set(status.get(name, {}).get("layout_rules_waived", {}))

    pro = json.loads((KICAD / name / f"{name}.kicad_pro").read_text())
    h2h = pro["board"]["design_settings"]["rules"]["min_hole_to_hole"]
    if h2h < 0.5:
        problems.append(f"min_hole_to_hole is {h2h} mm, must stay >= 0.5 (HD-039)")

    texts = [d for d in board.GetDrawings() if d.GetClass() == "PCB_TEXT"]
    if not any(t.GetText() == "JLCJLCJLCJLC" and t.GetLayer() == pcbnew.B_SilkS for t in texts):
        problems.append("JLCPCB order-number placeholder missing from the bottom silk (M-04)")
    printed = {t.GetText() for t in texts if t.GetLayer() in (pcbnew.F_SilkS, pcbnew.B_SilkS)}
    missing = sorted(f"{ref}={txt}" for ref, txt in LABELS.get(name, {}).items() if ref in fps and txt not in printed)
    if missing and "labels" not in waived:
        problems.append(f"silk labels not printed (S-01/S-03): {', '.join(missing)}")

    if name in HOLES:
        holes = [f for r, f in fps.items() if r.startswith("H") and f.IsBoardOnly()]
        if len(holes) != 4:
            problems.append(f"{len(holes)} mounting holes, expected 4 (M-01/M-02)")
        for h in holes:
            p = h.Pads()[0] if len(h.Pads()) else None
            if p is None or p.GetAttribute() != pcbnew.PAD_ATTRIB_NPTH or abs(mm(p.GetDrillSize().x) - HOLES[name]) > 0.01:
                problems.append(f"{h.GetReference()}: not an unplated {HOLES[name]} mm hole")

    if name == "TL-SN-001":
        for part, net, anchor, pins, limit in TEMPLATE_RULES:
            pad = _pad(fps[part], net)
            d = min(_dist(pad, _pad(fps[anchor], number=n)) for n in pins)
            if d > limit:
                problems.append(f"{part} {net} pad is {d:.2f} mm from {anchor} pin {'/'.join(pins)} "
                                f"(template allows {limit}) (HD-036/037)")
        for ic in ("U4", "U5"):
            ep = max(fps[ic].Pads(), key=lambda p: p.GetSize().x * p.GetSize().y)
            n = sum(1 for t in board.GetTracks() if t.GetClass() == "PCB_VIA"
                    and ep.HitTest(t.GetPosition()))
            if n < 2 and "thermal_vias" not in waived:
                problems.append(f"{ic} thermal pad has {n} via(s), needs >= 2 (HD-041)")
        for ref in LAMP_STRIP:
            gap = mm(edge.GetBottom() - fps[ref].GetPosition().y)
            if gap > 3.0:
                problems.append(f"{ref} is {gap:.1f} mm from the bottom edge: the lamp strip must stay on it (M-06)")

    if name == "TL-SN-002":
        for part, net, anchor, pins, limit in SN2_TEMPLATE:
            pad = _pad(fps[part], net)
            d = min(_dist(pad, _pad(fps[anchor], number=n)) for n in pins)
            if d > limit:
                problems.append(f"{part} {net} pad is {d:.2f} mm from {anchor} pin {'/'.join(pins)} "
                                f"(template allows {limit}) (HD-036)")
        ep = max(fps["U5"].Pads(), key=lambda p: p.GetSize().x * p.GetSize().y)
        n = sum(1 for t in board.GetTracks() if t.GetClass() == "PCB_VIA" and ep.HitTest(t.GetPosition()))
        if n < 2 and "thermal_vias" not in waived:
            problems.append(f"U5 thermal pad has {n} via(s), needs >= 2 (HD-041)")
        for ref in SN2_LAMPS:
            gap = mm(fps[ref].GetPosition().x - edge.GetX())
            if gap > 3.0:
                problems.append(f"{ref} is {gap:.1f} mm from the left edge: the lamp strip must stay on it (M-06)")
        import gen_pcbs as _g  # tongue geometry lives with the generator
        tx0 = min(c[0] for c in _g.SN2_TONGUE_SLOTS)
        ty0, ty1 = _g.SN2_TONGUE_SLOTS[0][3], _g.SN2_TONGUE_SLOTS[1][1]
        x, y = (mm(v) - o for v, o in zip((fps["U9"].GetPosition().x, fps["U9"].GetPosition().y), (_g.ORIGIN)))
        if not (x > tx0 and ty0 < y < ty1):
            problems.append(f"SHT40 U9 at ({x:.1f}, {y:.1f}) is off the thermal tongue")
        gap = mm(edge.GetRight() - fps["U6"].GetPosition().x)
        if gap > 3.0:
            problems.append(f"Hall sensor U6 is {gap:.1f} mm from the right edge (it must face the door magnet)")

    if name in CARDS:
        j1 = fps["J1"]
        lowest = max(p.GetBoundingBox().GetBottom() for p in j1.Pads())
        pull = mm(edge.GetBottom() - lowest)
        if pull < 0.6:
            problems.append(f"finger copper ends {pull:.2f} mm from the card edge, needs >= 0.6 (HD-040)")

    ns = board.GetDesignSettings().m_NetSettings
    narrow = 0.0
    for t in board.GetTracks():
        if t.GetClass() != "PCB_TRACK":
            continue
        nc = ns.GetEffectiveNetClass(t.GetNetname())
        if nc.GetName() in ("Power", "Rail") and t.GetWidth() < nc.GetTrackWidth() - pcbnew.FromMM(0.005):
            narrow += mm(t.GetLength())
    budget = status.get(name, {}).get("narrow_power_mm_max", 0.0)
    if narrow > budget + 1e-6:
        problems.append(f"{narrow:.1f} mm of Power/Rail track below its netclass width, budget {budget} mm (HD-038)")
    return problems


def main() -> int:
    status = json.loads((KICAD / "layout-status.json").read_text())
    ok = True
    for name in sys.argv[1:] or BOARDS:
        problems = check(name, status)
        print(f"{name}: {'ok' if not problems else 'FAIL'}")
        for p in problems:
            print(f"  - {p}")
        ok &= not problems
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
