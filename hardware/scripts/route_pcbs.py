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
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

import pcbnew

ROOT = pathlib.Path(__file__).resolve().parents[2]
KICAD = ROOT / "hardware" / "kicad"
BOARDS = ["TL-SN-001", "TL-MOD-001", "TL-MOD-002", "TL-MOD-003", "TL-MOD-004", "TL-TEST-001"]
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


def route(name: str, attempt: int = 1) -> None:
    path = KICAD / name / f"{name}.kicad_pcb"
    board = pcbnew.LoadBoard(str(path))
    # Copper pours on signal layers would be exported as planes, and Freerouting would then skip
    # their nets. Hide them so every net gets real tracks; the pours come back as stitching after.
    signal_pours = [z for z in board.Zones() if not z.GetIsRuleArea()
                    and board.GetLayerType(z.GetLayer()) != pcbnew.LT_POWER]
    for z in signal_pours:
        board.Remove(z)
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
    try:
        for attempt in range(1, ATTEMPTS + 1):
            shutil.copy(placed, path)
            route(name, attempt)
            errors = drc_errors(path)
            print(f"  attempt {attempt}: {len(errors)} DRC errors")
            if not errors:
                return True
            for e in errors[:5]:
                print(f"    {e}")
        return False
    finally:
        placed.unlink()


def main() -> int:
    ok = True
    for name in sys.argv[1:] or BOARDS:
        ok &= route_until_clean(name)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
