"""Renders the TL-VN-001 housing with stand-in parts (matplotlib, no GPU needed).

    ~/.cache/talos-tools/venv-cad/bin/python hardware/mechanical/vision_node/render.py

Writes out/vn-exploded.png (lid pulled back, mount plate lifted) and out/vn-front.png.
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


def tris(shape, dy=0.0, dz=0.0):
    verts, faces = shape.val().tessellate(0.3, 0.4)
    pts = [(v.x, v.y + dy, v.z + dz) for v in verts]
    return [[pts[a], pts[b], pts[c]] for a, b, c in faces]


def stand_ins():
    """Camera, IR boards, Pi stack, switch: boxes at their design positions."""
    lx, lz = H.lens_x(), H.LENS_Z
    cam_top = lz + H.CAM_LENS_FROM_TOP
    cam = cq.Workplane("XY").box(H.CAM[0], 1.6, H.CAM[1], centered=(True, False, False)).translate(
        (lx, H.CAM_DEPTH - 1.6, cam_top - H.CAM[1]))
    lens = cq.Workplane("XZ").center(lx, lz).circle(4.0).extrude(-(H.CAM_DEPTH - 2.0)).translate((0, 0.5, 0))
    irs = [cq.Workplane("XY").box(H.IR_BOARD, 1.6, H.IR_BOARD, centered=(True, False, True)).translate(
        (lx + s * H.IR_OFFSET, 4.0, lz)) for s in (-1, 1)]
    hx, hy = H.zero_holes()[0]
    stack = cq.Workplane("XY").box(H.ZERO[0], H.ZERO[1], H.STACK_H, centered=False).translate(
        (hx - H.ZERO_HOLES, hy - H.ZERO_HOLES, 0))
    sx, sy = H.switch_xy()
    sw = cq.Workplane("XY").box(H.SWITCH[0], H.SWITCH[1], H.SWITCH[2], centered=(True, True, False)).translate(
        (sx, sy, 0))
    return cam, lens, irs, stack, sw


def draw(ax, shape, color, dy=0.0, dz=0.0, alpha=0.5):
    ax.add_collection3d(Poly3DCollection(tris(shape, dy, dz), facecolor=color, edgecolor="none", alpha=alpha))


def main() -> int:
    body, lid, plate = H.body(), H.lid(), H.plate()
    cam, lens, irs, stack, sw = stand_ins()
    ow, od, oh = H.outer()
    w, d, h = H.INNER
    plate_at = plate.rotate((0, 0, 0), (1, 0, 0), 180).translate((H.lens_x(), d / 2, h + H.WALL + 46))
    for name, explode, elev, azim in (("vn-exploded", True, 22, -58), ("vn-front", False, 8, -90)):
        fig = plt.figure(figsize=(11, 8), dpi=110)
        ax = fig.add_subplot(111, projection="3d")
        draw(ax, body, "#8fa3b8", alpha=0.35)
        draw(ax, cam, "#2f7d32", alpha=0.95)
        draw(ax, lens, "#111111", alpha=0.95)
        for ir in irs:
            draw(ax, ir, "#7a1f1f", alpha=0.9)
        draw(ax, stack, "#2f7d32", alpha=0.8)
        draw(ax, sw, "#222222", alpha=0.9)
        draw(ax, lid, "#c9b37e", dy=40.0 if explode else 0.0, alpha=0.6)
        if explode:
            draw(ax, plate_at, "#6c757d", alpha=0.6)
        ax.set_xlim(-10, ow + 10)
        ax.set_ylim(-20, od + (50 if explode else 10))
        ax.set_zlim(-10, oh + (80 if explode else 10))
        ax.set_box_aspect((ow + 20, od + (70 if explode else 30), oh + (90 if explode else 20)))
        ax.view_init(elev=elev, azim=azim)
        ax.set_axis_off()
        ax.set_title(f"TL-VN-001 housing {ow:.0f} x {od:.0f} x {oh:.0f} mm (PROVISIONAL)")
        out = H.OUT / f"{name}.png"
        H.OUT.mkdir(exist_ok=True)
        fig.savefig(out, bbox_inches="tight")
        plt.close(fig)
        print("wrote", out.relative_to(H.HERE.parents[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
