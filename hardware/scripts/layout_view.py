"""Block map of a placed or routed board: each part's courtyard coloured by its functional block.

    python3 hardware/scripts/layout_view.py BOARD.kicad_pcb OUT.png [--groups GROUPS.json] [--tracks]

Reads the board with KiCad's Python (a subprocess, so this script itself runs under any Python with
matplotlib) and draws courtyards, refs and, optionally, tracks. It is the picture used to judge a
layout's structure: do blocks stay together, are the four slots laid out alike, where are the gaps.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
KPY = os.environ.get("KICAD_PYTHON", os.path.expanduser(
    "~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3"))

DUMP = r'''
import json, sys, pcbnew
b = pcbnew.LoadBoard(sys.argv[1])
mm = pcbnew.ToMM
out = {"parts": [], "tracks": [], "vias": [], "edge": []}
for fp in b.GetFootprints():
    box = None
    for layer in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
        poly = fp.GetCourtyard(layer)
        if poly.OutlineCount():
            bb = poly.BBox()
            box = bb if box is None else (box.Merge(bb) or box)
    if box is None:
        box = fp.GetBoundingBox(False)
    out["parts"].append({"ref": fp.GetReference(), "box": [mm(box.GetX()), mm(box.GetY()), mm(box.GetRight()),
                         mm(box.GetBottom())], "back": fp.GetLayer() == pcbnew.B_Cu})
for t in b.GetTracks():
    if t.GetClass() == "PCB_TRACK":
        out["tracks"].append([mm(t.GetStart().x), mm(t.GetStart().y), mm(t.GetEnd().x), mm(t.GetEnd().y),
                              b.GetLayerName(t.GetLayer())])
    elif t.GetClass() == "PCB_VIA":
        out["vias"].append([mm(t.GetPosition().x), mm(t.GetPosition().y)])
for d in b.GetDrawings():
    if d.GetLayer() == pcbnew.Edge_Cuts:
        out["edge"].append([mm(d.GetStart().x), mm(d.GetStart().y), mm(d.GetEnd().x), mm(d.GetEnd().y)])
print(json.dumps(out))
'''

PALETTE = ["#4e79a7", "#f28e2b", "#e15759", "#76b7b2", "#59a14f", "#edc948", "#b07aa1", "#ff9da7",
           "#9c755f", "#bab0ac", "#86bcb6", "#d37295"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("out")
    ap.add_argument("--groups")
    ap.add_argument("--tracks", action="store_true")
    ap.add_argument("--title", default="")
    a = ap.parse_args()
    env = dict(os.environ, PYTHONNOUSERSITE="1")
    d = json.loads(subprocess.run([KPY, "-c", DUMP, a.board], env=env, capture_output=True, text=True,
                                  check=True).stdout)
    group_of, names = {}, []
    gpath = a.groups or str(pathlib.Path(a.board).with_suffix("").as_posix()) + ".groups.json"
    if os.path.exists(gpath):
        for g in json.load(open(gpath))["groups"]:
            names.append(g["name"])
            for r in g["refs"]:
                group_of[r] = g["name"]
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.patches as mp
    import matplotlib.pyplot as plt
    xs = [v for e in d["edge"] for v in (e[0], e[2])]
    ys = [v for e in d["edge"] for v in (e[1], e[3])]
    fig, ax = plt.subplots(figsize=(12, 12 * (max(ys) - min(ys)) / max(1, max(xs) - min(xs)) + 0.8), dpi=110)
    if a.tracks:
        for x0, y0, x1, y1, layer in d["tracks"]:
            ax.plot([x0, x1], [y0, y1], color="#c0392b" if layer == "F.Cu" else "#2e86c1", lw=0.5, alpha=0.55)
        for x, y in d["vias"]:
            ax.add_patch(mp.Circle((x, y), 0.3, color="#555", lw=0))
    for e in d["edge"]:
        ax.plot([e[0], e[2]], [e[1], e[3]], color="black", lw=1.2)
    for p in d["parts"]:
        g = group_of.get(p["ref"])
        col = PALETTE[names.index(g) % len(PALETTE)] if g else "#cccccc"
        x0, y0, x1, y1 = p["box"]
        ax.add_patch(mp.Rectangle((x0, y0), x1 - x0, y1 - y0, facecolor=col, alpha=0.45, edgecolor=col, lw=0.8))
        if x1 - x0 > 1.6 or y1 - y0 > 1.6:
            ax.text((x0 + x1) / 2, (y0 + y1) / 2, p["ref"], ha="center", va="center", fontsize=4.5)
    handles = [mp.Patch(color=PALETTE[i % len(PALETTE)], alpha=0.6, label=n) for i, n in enumerate(names)]
    if handles:
        ax.legend(handles=handles, loc="upper left", bbox_to_anchor=(1.0, 1.0), fontsize=8)
    ax.set_aspect("equal")
    ax.invert_yaxis()
    ax.set_xlim(min(xs) - 3, max(xs) + 3)
    ax.set_ylim(max(ys) + 3, min(ys) - 3)
    ax.set_title(a.title or pathlib.Path(a.board).name)
    ax.axis("off")
    fig.savefig(a.out, bbox_inches="tight")
    return 0


if __name__ == "__main__":
    sys.exit(main())
