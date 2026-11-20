"""Keep hand tweaks across regeneration: capture locked footprints into <board>.placement.json.

    # 1. open hardware/kicad/<board>/<board>.kicad_pcb in KiCad, move parts, lock them (L key)
    # 2. capture (KiCad's Python):
    python3 hardware/scripts/layout_tweaks.py capture TL-SN-001
    # 3. regenerate: gen_pcbs.py pins every captured part where you put it; the placer works around them
    python3 hardware/scripts/layout_tweaks.py clear TL-SN-001 [REF ...]   # forget some or all

The file is plain JSON ({ref: [x_mm, y_mm, rot_deg]} in board coordinates, origin at the top-left
corner of the outline), so it reviews well in a diff. Captured positions win over the spec's starting
positions and are never moved by the placer, the slot copy or the two-pad flip.
"""

from __future__ import annotations

import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
KICAD = ROOT / "hardware" / "kicad"
ORIGIN = (100.0, 100.0)  # must match gen_pcbs.py


def path_for(board: str) -> pathlib.Path:
    return KICAD / board / f"{board}.placement.json"


def load(board: str) -> dict[str, list[float]]:
    p = path_for(board)
    return json.load(open(p))["parts"] if p.exists() else {}


def save(board: str, parts: dict) -> None:
    path_for(board).write_text(json.dumps(
        {"note": "Hand-placed parts captured by hardware/scripts/layout_tweaks.py; gen_pcbs.py pins them.",
         "parts": dict(sorted(parts.items()))}, indent=1) + "\n")


def capture(board: str) -> int:
    import pcbnew
    b = pcbnew.LoadBoard(str(KICAD / board / f"{board}.kicad_pcb"))
    parts = load(board)
    n = 0
    for fp in b.GetFootprints():
        if fp.IsLocked() and not fp.IsBoardOnly():
            p = fp.GetPosition()
            parts[fp.GetReference()] = [round(pcbnew.ToMM(p.x) - ORIGIN[0], 3), round(pcbnew.ToMM(p.y) - ORIGIN[1], 3),
                                        int(round(fp.GetOrientationDegrees())) % 360]
            n += 1
    save(board, parts)
    print(f"{board}: {n} locked part(s) captured; {len(parts)} pinned in {path_for(board).name}")
    return 0


def clear(board: str, refs: list[str]) -> int:
    parts = load(board)
    for r in refs or list(parts):
        parts.pop(r, None)
    if parts:
        save(board, parts)
    elif path_for(board).exists():
        path_for(board).unlink()
    print(f"{board}: {len(parts)} pinned part(s) left")
    return 0


def main() -> int:
    if len(sys.argv) < 3 or sys.argv[1] not in ("capture", "clear"):
        print(__doc__)
        return 2
    return capture(sys.argv[2]) if sys.argv[1] == "capture" else clear(sys.argv[2], sys.argv[3:])


if __name__ == "__main__":
    sys.exit(main())
