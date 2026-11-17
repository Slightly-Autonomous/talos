#!/usr/bin/env python3
"""Export a fabrication package for each routed board with kicad-cli.

    python3 hardware/scripts/export_fab.py [BOARD ...]

Writes hardware/fab/<board>/: Gerbers + Excellon drill (zipped), pick-and-place CSV, BOM CSV (from
hardware/results/bom), JLCPCB-format BOM/CPL plus a not-assembled list (jlc_sourcing.py), and
top/bottom 3D renders. This is a *prototype* package for quoting and DFM
review only. Nothing is ordered (HW-0005): ordering needs explicit owner approval.
"""

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jlc_sourcing  # noqa: E402
from check_kicad import BOARDS, KICAD, RESULTS, ROOT, kicad_cli  # noqa: E402

FAB = ROOT / "hardware" / "fab"


def run(cmd: list[str]) -> None:
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit(f"failed: {' '.join(cmd)}\n{p.stdout}\n{p.stderr}")


def export(cli: str, board: str) -> list[str]:
    pcb = KICAD / board / f"{board}.kicad_pcb"
    out = FAB / board
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    with tempfile.TemporaryDirectory() as tmp:
        g = pathlib.Path(tmp) / "gerbers"
        g.mkdir()
        inner = "In1.Cu,In2.Cu," if board == "TL-SN-001" else ""
        layers = f"F.Cu,{inner}B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts"
        run([cli, "pcb", "export", "gerbers", "--layers", layers, "--no-protel-ext", "--subtract-soldermask",
             "--use-drill-file-origin", "-o", str(g), str(pcb)])
        run([cli, "pcb", "export", "drill", "--format", "excellon", "--excellon-separate-th", "--drill-origin", "plot",
             "--generate-map", "--map-format", "gerberx2", "-o", str(g) + "/", str(pcb)])
        with zipfile.ZipFile(out / f"{board}-gerbers.zip", "w", zipfile.ZIP_DEFLATED) as z:
            for f in sorted(g.iterdir()):
                info = zipfile.ZipInfo(f.name, date_time=(2026, 1, 1, 0, 0, 0))  # stable zip for clean diffs
                z.writestr(info, _strip_dates(f.read_text(errors="replace")))
    run([cli, "pcb", "export", "pos", "--format", "csv", "--units", "mm", "--side", "both", "--use-drill-file-origin",
         "--exclude-dnp", "-o", str(out / f"{board}-cpl.csv"), str(pcb)])
    shutil.copy(RESULTS / "bom" / f"{board}.csv", out / f"{board}-bom.csv")
    jlc_sourcing.export([board])  # JLCPCB-format BOM/CPL + not-assembled list
    for side in ("top", "bottom"):
        run([cli, "pcb", "render", "--side", side, "--width", "1200", "--height", "900", "--quality", "basic",
             "-o", str(out / f"{board}-{side}.png"), str(pcb)])
    return sorted(p.name for p in out.iterdir())


def _strip_dates(text: str) -> str:
    """Drop generator timestamps so regenerated Gerbers diff cleanly."""
    keep = []
    for line in text.splitlines():
        if "CreationDate" in line or line.startswith("; #@! TF.CreationDate") or line.startswith(";DRILL file {KiCad"):
            continue
        keep.append(line)
    return "\n".join(keep) + "\n"


def main() -> int:
    cli = kicad_cli()
    for b in sys.argv[1:] or BOARDS:
        print(f"{b}: {', '.join(export(cli, b))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
