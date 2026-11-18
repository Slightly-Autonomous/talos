"""Autoroute the generated PCBs with Freerouting (Specctra DSN/SES round trip), refill zones, save.

Run with KiCad's bundled Python:
    FREEROUTING_JAR=~/.cache/talos-tools/freerouting-2.5.0.jar JAVA=/usr/local/opt/openjdk/bin/java \\
    ~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 \\
        hardware/scripts/route_pcbs.py [BOARD ...]

Freerouting is a general-purpose autorouter: the result is a connectivity-correct first pass, not a
reviewed layout (no impedance control, no deliberate RF or power-loop placement).
"""

from __future__ import annotations

import json
import math
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

import pcbnew

ROOT = pathlib.Path(__file__).resolve().parents[2]
KICAD = ROOT / "hardware" / "kicad"
BOARDS = ["TL-SN-001", "TL-MOD-001", "TL-MOD-002", "TL-MOD-003", "TL-MOD-004", "TL-TEST-001", "TL-DBG-001"]
JAR = os.path.expanduser(os.environ.get("FREEROUTING_JAR", "~/.cache/talos-tools/freerouting-2.5.0.jar"))
JAVA = os.environ.get("JAVA", "java")
# macOS app bundle launcher (bundles its own Java runtime); used when set.
APP = os.path.expanduser(os.environ.get("FREEROUTING_APP", ""))
PASSES = os.environ.get("FREEROUTING_PASSES", "200")
ATTEMPTS = int(os.environ.get("ROUTE_ATTEMPTS", "6"))
KICAD_CLI = os.environ.get("KICAD_CLI", os.path.expanduser("~/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"))


def drc_errors(path: pathlib.Path) -> list[str]:
    """kicad-cli DRC; returns error-severity findings (warnings such as silkscreen are reported, not fatal)."""
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "drc.json")
        subprocess.run([KICAD_CLI, "pcb", "drc", "--schematic-parity", "--refill-zones", "--format", "json",
                        "--severity-all", "-o", out, str(path)], capture_output=True)
        d = json.load(open(out))
    found = []
    for key in ("violations", "unconnected_items", "schematic_parity"):
        for v in d.get(key, []):
            if v["severity"] == "error":
                found.append(f"{v['type']}: {v['description']}")
    return found


def remove_dangling_vias(board) -> int:
    """Drop router leftovers: vias that no track or pad reaches on at least two copper layers."""
    tracks = [t for t in board.GetTracks() if t.GetClass() == "PCB_TRACK"]
    pads = list(board.GetPads())
    dangling = []
    for via in [t for t in board.GetTracks() if t.GetClass() == "PCB_VIA"]:
        pos, net = via.GetPosition(), via.GetNetCode()
        layers = set()
        radius = via.GetWidth(pcbnew.F_Cu) // 2
        for t in tracks:
            if t.GetNetCode() != net:
                continue
            if any((e - pos).EuclideanNorm() <= radius for e in (t.GetStart(), t.GetEnd())) or t.HitTest(pos):
                layers.add(t.GetLayer())
        for pad in pads:
            if pad.GetNetCode() == net and pad.HitTest(pos):
                layers.update(l for l in pad.GetLayerSet().Seq() if pcbnew.IsCopperLayer(l))
        plane = any(board.GetLayerType(z.GetLayer()) == pcbnew.LT_POWER and z.GetNetCode() == net
                    for z in board.Zones() if not z.GetIsRuleArea())
        if len(layers) + (1 if plane else 0) < 2:
            dangling.append(via)
    for via in dangling:
        board.Delete(via)
    return len(dangling)


def _rect_dist(px, py, box) -> float:
    x0, y0, x1, y1 = box
    dx = max(x0 - px, 0, px - x1)
    dy = max(y0 - py, 0, py - y1)
    return math.hypot(dx, dy)


def plane_fanout(board, via_d=0.6, drill=0.3, clear=0.2, track_w=0.25, stub_clear=0.17) -> int:
    """Give every SMD pad on a plane net (GND on In1, V3V3_CORE on In2) its own via before autorouting.

    Freerouting fans plane pins out poorly (it left >100 GND pins unconnected and spent minutes per
    pass on them). A deterministic, clearance-checked stub + via per pad removes the plane net from the
    router's job. Pads larger than the via (exposed thermal pads) get the via in the pad: that needs a
    plugged/tented via at fab (review item).
    """
    planes = [z for z in board.Zones() if not z.GetIsRuleArea() and board.GetLayerType(z.GetLayer()) == pcbnew.LT_POWER]
    plane_nets = sorted({z.GetNetCode() for z in planes})
    to = pcbnew.ToMM
    edge = board.GetBoardEdgesBoundingBox()
    ex0, ey0, ex1, ey1 = to(edge.GetX()), to(edge.GetY()), to(edge.GetRight()), to(edge.GetBottom())
    rule_areas = [z for z in board.Zones() if z.GetIsRuleArea()]
    rule_areas += [z for fp in board.Footprints() for z in fp.Zones() if z.GetIsRuleArea()]
    no_via = [z for z in rule_areas if z.GetDoNotAllowVias()]
    no_track = [z for z in rule_areas if z.GetDoNotAllowTracks()]

    def blocked(zones, x, y, r, layer=None) -> bool:
        pts = [(x, y)] + [(x + r * math.cos(k * math.pi / 4), y + r * math.sin(k * math.pi / 4)) for k in range(8)]
        for z in zones:
            if layer is not None and not z.IsOnLayer(layer):
                continue
            if any(z.Outline().Contains(pcbnew.VECTOR2I(pcbnew.FromMM(a), pcbnew.FromMM(b))) for a, b in pts):
                return True
        return False

    pads = []
    for p in board.GetPads():
        bb = p.GetBoundingBox()
        pads.append((p, (to(bb.GetX()), to(bb.GetY()), to(bb.GetRight()), to(bb.GetBottom()))))
    vias: list[tuple[float, float]] = []
    added = 0
    for pad, box in [(p, b) for p, b in pads if p.GetNetCode() in plane_nets]:
        net = pad.GetNetCode()
        if pad.GetAttribute() != pcbnew.PAD_ATTRIB_SMD:
            continue
        layer = pcbnew.F_Cu if pad.IsOnLayer(pcbnew.F_Cu) else pcbnew.B_Cu
        px, py = to(pad.GetPosition().x), to(pad.GetPosition().y)
        w, h = box[2] - box[0], box[3] - box[1]
        stub_w = max(0.15, min(track_w, w, h))  # fine-pitch pads (UQFN) get a stub no wider than the pad

        def via_ok(vx, vy, own=pad) -> bool:
            r = via_d / 2
            if not (ex0 + 0.8 < vx < ex1 - 0.8 and ey0 + 0.8 < vy < ey1 - 0.8):
                return False
            if blocked(no_via, vx, vy, r + 0.05):
                return False
            for q, qb in pads:
                if q.GetNetCode() == net and (q is own or _rect_dist(vx, vy, qb) > 0):
                    continue
                if q.GetNetCode() != net and _rect_dist(vx, vy, qb) < r + clear:
                    return False
                if q.GetNetCode() == net and q is not own:  # via inside another GND pad is fine but pointless
                    return False
            return all(math.hypot(vx - ax, vy - ay) >= via_d + clear for ax, ay in vias)

        def stub_ok(vx, vy) -> bool:
            for k in range(1, 11):
                sx, sy = px + (vx - px) * k / 10, py + (vy - py) * k / 10
                if blocked(no_track, sx, sy, stub_w / 2 + 0.05, layer):
                    return False
                for q, qb in pads:
                    if q.GetNetCode() != net and q.IsOnLayer(layer) and _rect_dist(sx, sy, qb) < stub_w / 2 + stub_clear:
                        return False
            return True

        spot = None
        if min(w, h) >= via_d + 0.1 and via_ok(px, py):  # thermal pad: via in pad
            spot = (px, py)
        else:
            fp = pad.GetParentFootprint()
            cx, cy = to(fp.GetPosition().x), to(fp.GetPosition().y)
            # Leave straight out of the pad's free end (perpendicular to the package edge) first:
            # a diagonal from the footprint centre clips the neighbouring pins at 0.4 mm pitch.
            if abs(h - w) > 0.05:
                base = (math.pi / 2 if py > cy else -math.pi / 2) if h > w else (0.0 if px > cx else math.pi)
            else:
                base = math.atan2(py - cy, px - cx) if (px, py) != (cx, cy) else -math.pi / 2
            for dist in [max(w, h) / 2 + via_d / 2 + clear + 0.1 * k for k in range(30)]:
                for da in (0, 30, -30, 60, -60, 90, -90, 120, -120, 150, -150, 180):
                    a = base + math.radians(da)
                    vx, vy = round(px + dist * math.cos(a), 3), round(py + dist * math.sin(a), 3)
                    if via_ok(vx, vy) and stub_ok(vx, vy):
                        spot = (vx, vy)
                        break
                if spot:
                    break
        if not spot:
            print(f"    plane fanout: no via spot for {pad.GetParentFootprint().GetReference()}-{pad.GetNumber()}")
            continue
        vx, vy = spot
        via = pcbnew.PCB_VIA(board)
        via.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(vx), pcbnew.FromMM(vy)))
        via.SetWidth(pcbnew.FromMM(via_d))
        via.SetDrill(pcbnew.FromMM(drill))
        via.SetNetCode(net)
        via.SetLocked(True)
        board.Add(via)
        if (vx, vy) != (px, py):
            t = pcbnew.PCB_TRACK(board)
            t.SetStart(pad.GetPosition())
            t.SetEnd(via.GetPosition())
            t.SetWidth(pcbnew.FromMM(stub_w))
            t.SetLayer(layer)
            t.SetNetCode(net)
            t.SetLocked(True)
            board.Add(t)
        vias.append(spot)
        added += 1
    return added


def route(name: str, attempt: int = 1) -> None:
    path = KICAD / name / f"{name}.kicad_pcb"
    board = pcbnew.LoadBoard(str(path))
    # Copper pours on signal layers would be exported as planes, and Freerouting would then skip
    # their nets. Hide them so every net gets real tracks; the pours come back as stitching after.
    signal_pours = [z for z in board.Zones() if not z.GetIsRuleArea()
                    and board.GetLayerType(z.GetLayer()) != pcbnew.LT_POWER]
    for z in signal_pours:
        board.Remove(z)
    fanned = plane_fanout(board)
    if fanned:
        print(f"    plane fanout: {fanned} pads given a plane via")
    # Route with extra clearance (grows per attempt): absorbs DSN rounding and varies the input
    # between attempts. Only the DSN sees it; DRC still checks the project's netclass value.
    nc = board.GetDesignSettings().m_NetSettings.GetDefaultNetclass()
    nc.SetClearance(pcbnew.FromMM(0.15 + 0.01 * attempt))
    with tempfile.TemporaryDirectory() as tmp:
        dsn, ses = os.path.join(tmp, f"{name}.dsn"), os.path.join(tmp, f"{name}.ses")
        if not pcbnew.ExportSpecctraDSN(board, dsn):
            raise SystemExit(f"{name}: DSN export failed")
        args = ["-de", dsn, "-do", ses, "-mp", PASSES, "--gui.enabled=false"]
        cmd = [APP] + args if APP else [JAVA, "-jar", JAR] + args
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=5400)
        summary = [ln for ln in (p.stdout + p.stderr).splitlines()
                   if any(k in ln.lower() for k in ("unrouted", "incomplete", "completed", "pass #"))]
        for ln in summary[-6:]:
            print(f"    freerouting: {ln.strip()[:160]}")
        if not os.path.exists(ses):
            print(p.stdout[-3000:], p.stderr[-3000:])
            raise SystemExit(f"{name}: Freerouting produced no session file")
        if not pcbnew.ImportSpecctraSES(board, ses):
            raise SystemExit(f"{name}: SES import failed")
    nc.SetClearance(pcbnew.FromMM(0.15))  # restore before saving; DRC must see the real rule
    for z in signal_pours:
        board.Add(z)
    for t in board.GetTracks():  # Freerouting necks down at fine-pitch pads; hold the 0.1 mm fab minimum
        if t.GetClass() == "PCB_TRACK" and t.GetWidth() < pcbnew.FromMM(0.1):
            t.SetWidth(pcbnew.FromMM(0.1))
    removed = 0 if os.environ.get("ROUTE_KEEP_VIAS") else remove_dangling_vias(board)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(str(path))
    tracks = sum(1 for t in board.GetTracks() if t.GetClass() == "PCB_TRACK")
    vias = sum(1 for t in board.GetTracks() if t.GetClass() == "PCB_VIA")
    print(f"{name}: {tracks} track segments, {vias} vias ({removed} dangling vias removed)")


def route_until_clean(name: str) -> bool:
    """Freerouting is not deterministic; retry from the placed board until DRC has no errors."""
    path = KICAD / name / f"{name}.kicad_pcb"
    placed = path.with_suffix(".placed")
    shutil.copy(path, placed)
    best = path.with_suffix(".best")
    best_count = None
    try:
        for attempt in range(1, ATTEMPTS + 1):
            shutil.copy(placed, path)
            route(name, attempt)
            errors = drc_errors(path)
            print(f"  attempt {attempt}: {len(errors)} DRC errors", flush=True)
            if not errors:
                return True
            for e in errors[:5]:
                print(f"    {e}")
            if best_count is None or len(errors) < best_count:
                best_count = len(errors)
                shutil.copy(path, best)
        shutil.copy(best, path)  # keep the best attempt, not the last
        print(f"  kept best attempt: {best_count} DRC errors")
        return False
    finally:
        placed.unlink()
        best.unlink(missing_ok=True)


def main() -> int:
    ok = True
    for name in sys.argv[1:] or BOARDS:
        ok &= route_until_clean(name)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
