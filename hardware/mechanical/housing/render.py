"""Exploded render of the TL-SN-002 housing with a stand-in board (matplotlib, no GPU needed).

    ~/.cache/talos-tools/venv-cad/bin/python hardware/mechanical/housing/render.py

Reads the parts housing.py builds and writes out/housing-exploded.png and out/housing-top.png.
"""

from __future__ import annotations

import pathlib
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from mpl_toolkits.mplot3d.art3d import Poly3DCollection  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import housing as H  # noqa: E402
import cadquery as cq  # noqa: E402


def tris(shape, dz=0.0):
    verts, faces = shape.val().tessellate(0.3, 0.4)
    pts = [(v.x, v.y, v.z + dz) for v in verts]
    return [[pts[a], pts[b], pts[c]] for a, b, c in faces]


def board_parts():
    z = H.z_levels()
    w, h = H.board_size()
    pcb = cq.Workplane("XY").box(w, h, H.PCB_T, centered=False).translate((H.CLEAR, H.CLEAR, z["pcb_bottom"]))
    bt = H.BOARD["battery_holder"]["courtyard"]
    holder = cq.Workplane("XY").box(bt[2] - bt[0], bt[3] - bt[1], 15.5, centered=False).translate(
        (H.bx(bt[0]), H.by(bt[3]), z["pcb_top"]))
    p = H.BOARD["pir"]
    pir = cq.Workplane("XY").circle(4.55).extrude(4.5).translate((H.bx(p["x"]), H.by(p["y"]), z["pcb_top"]))
    sws = [cq.Workplane("XY").box(5.1, 5.1, 1.5, centered=(True, True, False)).translate(
        (H.bx(s["x"]), H.by(s["y"]), z["pcb_top"])) for s in H.BOARD["tamper_switches"].values()]
    return pcb, holder, pir, sws


def draw(ax, shape, color, dz=0.0, alpha=0.55):
    ax.add_collection3d(Poly3DCollection(tris(shape, dz), facecolor=color, edgecolor="none", alpha=alpha))


def main() -> int:
    base, lid, door, magnet = H.base(), H.lid(), H.battery_door(), H.magnet_carrier()
    pcb, holder, pir, sws = board_parts()
    ow, oh = H.outer()
    for name, explode, elev, azim in (("housing-exploded", True, 24, -62), ("housing-top", False, 88, -90)):
        fig = plt.figure(figsize=(11, 8), dpi=110)
        ax = fig.add_subplot(111, projection="3d")
        e = (lambda k: k * 22.0) if explode else (lambda k: 0.0)
        draw(ax, base, "#8fa3b8", 0.0)
        draw(ax, pcb, "#2f7d32", e(1), 0.9)
        draw(ax, holder, "#444444", e(1), 0.8)
        draw(ax, pir, "#d8d8d8", e(1), 0.9)
        for s in sws:
            draw(ax, s, "#c0392b", e(1), 0.95)
        if explode:
            draw(ax, lid, "#e9e2d0", e(2), 0.35)
            draw(ax, door, "#e0a030", e(3), 0.8)
            draw(ax, magnet.translate((ow + 12.0, H.by(H.BOARD["hall"]["y"]), 0)), "#7b4fa0", 0.0, 0.8)
        else:
            draw(ax, lid, "#e9e2d0", 0.0, 0.45)
            draw(ax, door, "#e0a030", 0.0, 0.8)
        ax.set_xlim(-H.BAY - 5, ow + (40 if explode else 0))
        ax.set_ylim(-15, oh + 5)
        ax.set_zlim(0, 110 if explode else 40)
        ax.set_box_aspect((ow + (45 if explode else 5) + H.BAY, oh + 20, 110 if explode else 40))
        ax.view_init(elev=elev, azim=azim)
        ax.set_axis_off()
        ax.set_title("TL-SN-002 housing (PROVISIONAL): base, board stand-in, lid, battery door"
                     + (", magnet carrier" if explode else " (top view)"), fontsize=10)
        out = H.OUT / f"{name}.png"
        fig.savefig(out, bbox_inches="tight")
        plt.close(fig)
        print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
