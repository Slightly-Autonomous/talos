"""Autoroute the generated PCBs with Freerouting (Specctra DSN/SES round trip), refill zones, save.

Run with KiCad's bundled Python:
    FREEROUTING_JAR=~/.cache/talos-tools/freerouting-2.5.0.jar JAVA=/usr/local/opt/openjdk/bin/java \\
    ~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 \\
        hardware/scripts/route_pcbs.py [BOARD ...]

Attempts run in parallel (ROUTE_JOBS at a time), each in its own temp copy of the board folder; the
first DRC-clean attempt wins and the rest are stopped. If none is clean, a repair pass keeps the best
attempt's tracks, rips up any track in a clearance violation and stitches each connection that is
still open with a short clearance-checked track (seconds, instead of another ~30 min attempt).

After routing, silk_pass.py places every reference designator clear of pads and other silk.

Freerouting is a general-purpose autorouter: the result is a connectivity-correct first pass, not a
reviewed layout (no impedance control, no deliberate RF or power-loop placement).
"""

from __future__ import annotations

import json
import math
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time

import pcbnew

ROOT = pathlib.Path(__file__).resolve().parents[2]
KICAD = ROOT / "hardware" / "kicad"
# Layout experiments route a board in a copy of its project folder (layout_search.py); default: the repo.
BOARDS_DIR = pathlib.Path(os.environ.get("TALOS_LAYOUT_OUT", str(KICAD)))
BOARDS = ["TL-SN-001", "TL-MOD-001", "TL-MOD-002", "TL-MOD-003", "TL-MOD-004", "TL-TEST-001", "TL-DBG-001"]
JAR = os.path.expanduser(os.environ.get("FREEROUTING_JAR", "~/.cache/talos-tools/freerouting-2.5.0.jar"))
JAVA = os.environ.get("JAVA", "java")
# macOS app bundle launcher (bundles its own Java runtime); used when set.
APP = os.path.expanduser(os.environ.get("FREEROUTING_APP", ""))
# Freerouting never sees the Sensor Node as complete (it chases plane pins that the inner planes
# already connect), so it always runs every pass: 200 passes took ~39 min an attempt. The first
# passes do nearly all the work; the repair pass picks up what a short attempt leaves.
PASSES = os.environ.get("FREEROUTING_PASSES", "60")
REPAIR_ROUNDS = int(os.environ.get("REPAIR_ROUNDS", "5"))  # rip-up can take a few rounds to settle
ATTEMPTS = int(os.environ.get("ROUTE_ATTEMPTS", "6"))
# Each Freerouting JVM keeps a few cores busy and may take ~6 GB: a quarter of the cores (3 on a
# 12-core, 24 GB Mac) keeps the machine responsive.
JOBS = int(os.environ.get("ROUTE_JOBS", str(max(1, min(ATTEMPTS, (os.cpu_count() or 4) // 4)))))
KICAD_CLI = os.environ.get("KICAD_CLI", os.path.expanduser("~/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"))


def drc_report(path: pathlib.Path) -> list[dict]:
    """kicad-cli DRC; returns error-severity findings (warnings such as silkscreen are reported, not fatal)."""
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "drc.json")
        subprocess.run([KICAD_CLI, "pcb", "drc", "--schematic-parity", "--refill-zones", "--format", "json",
                        "--severity-all", "-o", out, str(path)], capture_output=True)
        d = json.load(open(out))
    return [v for key in ("violations", "unconnected_items", "schematic_parity")
            for v in d.get(key, []) if v["severity"] == "error"]


def drc_errors(path: pathlib.Path) -> list[str]:
    return [f"{v['type']}: {v['description']}" for v in drc_report(path)]


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


# Nets that leave a fine-pitch IC as wide current paths (gen_schematics.py Power/Rail netclasses).
POWER_NETS = {"/SYS_PATH", "/USB_VBUS", "/CHG_IN", "/PV_IN", "/PV_RAW", "/REG_LX1", "/REG_LX2", "/VBAT"}


def _crowds_power_pin(pad, plane_nets, radius_mm: float = 0.9) -> bool:
    """True for a fine-pitch plane pin sitting next to a power pin of the same IC.

    Its fanout via would land in the power pin's escape path: Freerouting then could not leave the
    TPS63900 switch node or the BQ25186 VBAT pin (this replaced the old hand edits in finish_sn.py).
    Such a pin is left to the router instead, which ties it to the exposed pad or a nearby plane via.
    """
    size = pad.GetSize()
    if min(pcbnew.ToMM(size.x), pcbnew.ToMM(size.y)) > 0.35:
        return False  # exposed pads and coarse pins keep their via
    pos = pad.GetPosition()
    for other in pad.GetParentFootprint().Pads():
        if other is pad or other.GetNetCode() in plane_nets or other.GetNetname() not in POWER_NETS:
            continue
        d = other.GetPosition() - pos
        if math.hypot(pcbnew.ToMM(d.x), pcbnew.ToMM(d.y)) <= radius_mm:
            return True
    return False


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
        if _crowds_power_pin(pad, plane_nets):
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


def plane_connected_pins(board) -> dict[str, set[str]]:
    """Plane-net pins the planes already reach, per DSN net name: through-hole pins, and SMD pads with a
    same-net track (the plane fanout stub) ending on them. Freerouting does not credit a connection made
    through a power plane, so it keeps chasing these pins: ~40 min an attempt on the Sensor Node."""
    planes = {z.GetNetname() for z in board.Zones()
              if not z.GetIsRuleArea() and board.GetLayerType(z.GetLayer()) == pcbnew.LT_POWER}
    ends: dict[int, list] = {}
    for t in board.GetTracks():
        if t.GetNetname() in planes:
            pts = [t.GetPosition()] if t.GetClass() == "PCB_VIA" else [t.GetStart(), t.GetEnd()]
            ends.setdefault(t.GetNetCode(), []).extend(pts)
    out: dict[str, set[str]] = {}
    for pad in board.GetPads():
        net = pad.GetNetname()
        if net not in planes:
            continue
        reached = pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH or any(
            pad.HitTest(pt) for pt in ends.get(pad.GetNetCode(), ()))
        if reached:
            out.setdefault(net, set()).add(f"{pad.GetParentFootprint().GetReference()}-{pad.GetNumber()}")
    return out


def drop_plane_pins(dsn: str, connected: dict[str, set[str]]) -> int:
    """Remove already-plane-connected pins from the DSN network, so Freerouting routes only real work.
    Duplicate-numbered pads (U1-49@3: the module's ground pads) go with their base pin."""
    import re
    text = open(dsn).read()
    dropped = 0

    def fix(m):
        nonlocal dropped
        name, body = m.group(1), m.group(2)
        done = connected.get(name, set())
        keep = [t for t in body.split() if t.split("@")[0] not in done]
        dropped += len(body.split()) - len(keep)
        return f"(net {name}\n      (pins {' '.join(keep)})"
    text = re.sub(r'\(net ("?[^\s()]+"?)\s*\(pins ([^()]*)\)', lambda m: fix(m) if m.group(1).strip('"') in connected
                  else m.group(0), text)
    open(dsn, "w").write(text)
    return dropped


def freeroute(board, name: str, passes: str, clearance_mm: float) -> int:
    """One Freerouting round trip on a loaded board; existing tracks are kept and extended."""
    # Copper pours on signal layers would be exported as planes, and Freerouting would then skip
    # their nets. Hide them so every net gets real tracks; the pours come back as stitching after.
    signal_pours = [z for z in board.Zones() if not z.GetIsRuleArea()
                    and board.GetLayerType(z.GetLayer()) != pcbnew.LT_POWER]
    for z in signal_pours:
        board.Remove(z)
    # Route with extra clearance: absorbs DSN rounding (and varies the input between attempts).
    # Only the DSN sees it; DRC still checks the project's netclass value.
    nc = board.GetDesignSettings().m_NetSettings.GetDefaultNetclass()
    nc.SetClearance(pcbnew.FromMM(clearance_mm))
    with tempfile.TemporaryDirectory() as tmp:
        dsn, ses = os.path.join(tmp, f"{name}.dsn"), os.path.join(tmp, f"{name}.ses")
        if not pcbnew.ExportSpecctraDSN(board, dsn):
            raise SystemExit(f"{name}: DSN export failed")
        if os.environ.get("ROUTE_PLANE_PINS") != "1":  # default: hide pins the planes already connect
            dropped = drop_plane_pins(dsn, plane_connected_pins(board))
            print(f"    dsn: {dropped} plane-connected pins hidden from Freerouting", flush=True)
        args = ["-de", dsn, "-do", ses, "-mp", passes, "--gui.enabled=false"]
        cmd = [APP] + args if APP else [JAVA, "-jar", JAR] + args
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=int(os.environ.get("FREEROUTING_TIMEOUT_S", "9000")))
        summary = [ln for ln in (p.stdout + p.stderr).splitlines()
                   if any(k in ln.lower() for k in ("unrouted", "incomplete", "completed", "pass #"))]
        for ln in summary[-4:]:
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
    return removed


def _counts(board) -> tuple[int, int]:
    tracks = sum(1 for t in board.GetTracks() if t.GetClass() == "PCB_TRACK")
    return tracks, sum(1 for t in board.GetTracks() if t.GetClass() == "PCB_VIA")


def route(path: pathlib.Path, name: str, attempt: int) -> None:
    """A fresh attempt from the placed board (worker process)."""
    board = pcbnew.LoadBoard(str(path))
    fanned = plane_fanout(board)
    if fanned:
        print(f"    plane fanout: {fanned} pads given a plane via")
    removed = freeroute(board, name, PASSES, 0.15 + 0.01 * attempt)
    board.Save(str(path))
    tracks, vias = _counts(board)
    print(f"{name}: {tracks} track segments, {vias} vias ({removed} dangling vias removed)")


def _by_uuid(board) -> dict:
    items = list(board.GetPads()) + list(board.GetTracks())
    return {i.m_Uuid.AsString(): i for i in items}


def _anchors(item, layer) -> list:
    """Points a stitch may start or end on, on one copper layer."""
    if item.GetClass() == "PCB_VIA":
        return [item.GetPosition()]
    if item.GetClass() in ("PCB_TRACK", "PCB_ARC"):
        return [item.GetStart(), item.GetEnd()] if item.GetLayer() == layer else []
    if item.GetClass() == "PAD":
        return [item.GetPosition()] if item.IsOnLayer(layer) else []
    return []


def _escape(pad):
    """A point straight out of the pad's free end, so the stitch leaves between its neighbours."""
    fp, pos, size = pad.GetParentFootprint(), pad.GetPosition(), pad.GetBoundingBox()
    w, h = size.GetWidth(), size.GetHeight()
    c = fp.GetPosition()
    if w > h:
        return pcbnew.VECTOR2I(pos.x + (1 if pos.x > c.x else -1) * (w // 2 + pcbnew.FromMM(0.3)), pos.y)
    return pcbnew.VECTOR2I(pos.x, pos.y + (1 if pos.y > c.y else -1) * (h // 2 + pcbnew.FromMM(0.3)))


def _paths(p, q, via_pad=None) -> list[list]:
    V = pcbnew.VECTOR2I
    out = [[p, q], [p, V(q.x, p.y), q], [p, V(p.x, q.y), q]]
    dx, dy = q.x - p.x, q.y - p.y
    d = min(abs(dx), abs(dy))
    sx, sy = (1 if dx > 0 else -1), (1 if dy > 0 else -1)
    out += [[p, V(p.x + sx * d, p.y + sy * d), q], [p, V(q.x - sx * d, q.y - sy * d), q]]  # 45 degree legs
    if via_pad is not None:
        e = _escape(via_pad)
        out += [[p] + path for path in _paths(e, q)]
    return out


def stitch(board, report: list[dict], margin_mm: float = 0.01) -> int:
    """Close leftover unconnected pairs with a short, clearance-checked track on a shared layer.

    Freerouting can leave the last connection of a crowded pin open (a VBAT pin in the charger's
    0.4 mm column). A full re-route to close one gap costs ~30 min; a straight or two-leg track out
    of the pad between its neighbours usually fits, and DRC re-checks whatever this adds.
    """
    F = pcbnew.FromMM
    edge = board.GetBoardEdgesBoundingBox()
    edge.Inflate(-F(0.3))
    keepouts = [z for z in board.Zones() if z.GetIsRuleArea() and z.GetDoNotAllowTracks()]
    keepouts += [z for fp in board.Footprints() for z in fp.Zones() if z.GetIsRuleArea() and z.GetDoNotAllowTracks()]
    copper = list(board.GetPads()) + [t for t in board.GetTracks()]
    fixed = 0
    by_uuid = _by_uuid(board)
    for v in report:
        if v["type"] != "unconnected_items" or len(v["items"]) != 2:
            continue
        a, b = (by_uuid.get(i["uuid"]) for i in v["items"])
        if a is None or b is None or a.GetNetCode() != b.GetNetCode():
            continue
        net = a.GetNetCode()
        nc = board.GetDesignSettings().m_NetSettings.GetEffectiveNetClass(a.GetNetname())
        clear = max(F(0.15), nc.GetClearance()) + F(margin_mm)
        widths = [w for w in (nc.GetTrackWidth(), F(0.25), F(0.2), F(0.15), F(0.1)) if w <= nc.GetTrackWidth()]
        done = False
        for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
            pads = [x for x in (a, b) if x.GetClass() == "PAD"]
            pairs = sorted(((pa, pb) for pa in _anchors(a, layer) for pb in _anchors(b, layer)),
                           key=lambda ab: (ab[0] - ab[1]).EuclideanNorm())
            others = [c for c in copper if c.GetNetCode() != net and c.IsOnLayer(layer)]
            for pa, pb in pairs[:4]:
                cands = _paths(pa, pb, pads[0] if pads and pads[0].GetPosition() == pa else None)
                if pads and pads[-1].GetPosition() == pb:
                    cands += [list(reversed(c)) for c in _paths(pb, pa, pads[-1])]
                cands.sort(key=lambda c: sum((c[i + 1] - c[i]).EuclideanNorm() for i in range(len(c) - 1)))
                for width in widths:
                    for path in cands:
                        segs = [(path[i], path[i + 1]) for i in range(len(path) - 1) if path[i] != path[i + 1]]
                        if any(not edge.Contains(pt) for pt in path):
                            continue
                        shapes = [pcbnew.SHAPE_SEGMENT(s0, s1, width) for s0, s1 in segs]
                        if any(o.GetEffectiveShape(layer).Collide(sh, clear) for o in others for sh in shapes):
                            continue
                        if any(z.IsOnLayer(layer) and z.Outline().Collide(sh, 0) for z in keepouts for sh in shapes):
                            continue
                        for s0, s1 in segs:
                            t = pcbnew.PCB_TRACK(board)
                            t.SetStart(s0)
                            t.SetEnd(s1)
                            t.SetWidth(width)
                            t.SetLayer(layer)
                            t.SetNetCode(net)
                            board.Add(t)
                            copper.append(t)
                        print(f"    stitch: {a.GetNetname()} on {board.GetLayerName(layer)}, "
                              f"{len(segs)} segment(s), {pcbnew.ToMM(width):.2f} mm")
                        done = True
                        break
                    if done:
                        break
                if done:
                    break
            if done:
                break
        if not done:  # needs a detour: grid maze route around the obstacles, small window first
            import maze_route
            for window, rip in ((8.0, False), (20.0, False), (20.0, True)):
                if maze_route.connect(board, a, b, window, rip):
                    print(f"    stitch: {a.GetNetname()} maze-routed (window {window:.0f} mm{', rip-up' if rip else ''})")
                    done = True
                    copper = list(board.GetPads()) + list(board.GetTracks())
                    break
        if done:
            fixed += 1
        else:
            print(f"    stitch: no clear path for {a.GetNetname()} near {v['items'][0]['pos']}")
    return fixed


def repair(path: pathlib.Path, name: str) -> None:
    """Keep the routed board: rip up tracks in clearance violations, then stitch open connections."""
    board = pcbnew.LoadBoard(str(path))
    ripped = 0
    by_uuid = _by_uuid(board)
    for v in drc_report(path):
        if v["type"] not in ("clearance", "tracks_crossing", "shorting_items"):
            continue
        for item in v["items"]:
            t = by_uuid.get(item["uuid"])
            if t is not None and t.GetClass() in ("PCB_TRACK", "PCB_ARC") and not t.IsLocked():
                board.Delete(t)
                ripped += 1
    if ripped:  # ripping opens connections; DRC names them so the stitch can close them
        board.Save(str(path))
    fixed = stitch(board, drc_report(path))
    remove_dangling_vias(board)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(str(path))
    print(f"{name}: repair ripped {ripped} segment(s), stitched {fixed} connection(s)")


def _workdir(tmp: pathlib.Path, tag: str, name: str, board: pathlib.Path) -> pathlib.Path:
    """Copy the board folder so each worker has its own project (DRC needs the .kicad_pro and rules)."""
    root = tmp / tag
    shutil.copytree(BOARDS_DIR / name, root / name)
    (root / "libraries").symlink_to(KICAD / "libraries")  # fp/sym tables use ${KIPRJMOD}/../libraries
    out = root / name / f"{name}.kicad_pcb"
    shutil.copy(board, out)
    return out


def _worker(mode: str, name: str, pcb: pathlib.Path, attempt: int, logs: dict) -> subprocess.Popen:
    log = open(pcb.parent.parent / "log.txt", "w")
    logs[pcb] = log
    return subprocess.Popen([sys.executable, __file__, f"--{mode}", name, str(pcb), str(attempt)],
                            stdout=log, stderr=subprocess.STDOUT, start_new_session=True)


def _stop(p: subprocess.Popen) -> None:
    if p.poll() is None:
        try:
            os.killpg(p.pid, signal.SIGTERM)  # takes Freerouting's JVM with it
        except ProcessLookupError:
            pass
        p.wait()


def _report(pcb: pathlib.Path) -> None:
    for ln in (pcb.parent.parent / "log.txt").read_text().splitlines():
        if ln.startswith(("TL-", "    ")) and "wx" not in ln and "Debug:" not in ln:
            print(ln)


def route_until_clean(name: str) -> bool:
    """Freerouting is not deterministic: run attempts in parallel, keep the first DRC-clean one.

    With no clean attempt, repair the best one instead of throwing its routing away.
    """
    path = BOARDS_DIR / name / f"{name}.kicad_pcb"
    with tempfile.TemporaryDirectory(prefix=f"route-{name}-") as tmpdir:
        tmp = pathlib.Path(tmpdir)
        placed = tmp / f"{name}.placed.kicad_pcb"
        shutil.copy(path, placed)
        pending = list(range(1, ATTEMPTS + 1))
        running: dict[subprocess.Popen, tuple[int, pathlib.Path]] = {}
        logs: dict = {}
        done: list[tuple[int, int, pathlib.Path]] = []  # (errors, attempt, pcb)
        try:
            while pending or running:
                while pending and len(running) < JOBS:
                    a = pending.pop(0)
                    pcb = _workdir(tmp, f"a{a}", name, placed)
                    running[_worker("attempt", name, pcb, a, logs)] = (a, pcb)
                    print(f"  attempt {a}: started", flush=True)
                finished = [p for p in running if p.poll() is not None]
                if not finished:
                    time.sleep(5)
                    continue
                for p in finished:
                    a, pcb = running.pop(p)
                    logs[pcb].close()
                    _report(pcb)
                    if p.returncode != 0:
                        print(f"  attempt {a}: worker failed (exit {p.returncode})", flush=True)
                        continue
                    errors = drc_errors(pcb)
                    print(f"  attempt {a}: {len(errors)} DRC errors", flush=True)
                    for e in errors[:5]:
                        print(f"    {e}")
                    if not errors:
                        shutil.copy(pcb, path)
                        return True
                    done.append((len(errors), a, pcb))
        finally:
            for p in running:
                _stop(p)
            for log in logs.values():
                log.close()
        if not done:
            raise SystemExit(f"{name}: every routing attempt failed")
        best_count, a, best = min(done)
        print(f"  best attempt {a}: {best_count} DRC errors; repairing", flush=True)
        for r in range(1, REPAIR_ROUNDS + 1):
            pcb = _workdir(tmp, f"r{r}", name, best)
            p = _worker("repair", name, pcb, r, logs)
            p.wait()
            logs[pcb].close()
            _report(pcb)
            if p.returncode != 0:
                print(f"  repair {r}: worker failed (exit {p.returncode})")
                break
            errors = drc_errors(pcb)
            print(f"  repair {r}: {len(errors)} DRC errors", flush=True)
            for e in errors[:5]:
                print(f"    {e}")
            if len(errors) < best_count:
                best_count, best = len(errors), pcb
            if not errors:
                break
        shutil.copy(best, path)  # keep the best result, not the last
        if best_count:
            print(f"  kept best result: {best_count} DRC errors")
        return best_count == 0


def main() -> int:
    if len(sys.argv) == 5 and sys.argv[1] in ("--attempt", "--repair"):
        name, pcb, n = sys.argv[2], pathlib.Path(sys.argv[3]), int(sys.argv[4])
        if sys.argv[1] == "--attempt":
            route(pcb, name, n)
        else:
            repair(pcb, name)
        return 0
    import silk_pass  # same folder; finishing pass on the routed board (references only, no copper)
    ok = True
    for name in sys.argv[1:] or BOARDS:
        ok &= route_until_clean(name)
        r = silk_pass.finish(str(BOARDS_DIR / name / f"{name}.kicad_pcb"))
        print(f"{name}: silkscreen {r['placed']} references placed, {r['fab_only']} on the fab layer only")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
