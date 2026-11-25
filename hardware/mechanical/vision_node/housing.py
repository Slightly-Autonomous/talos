"""TL-VN-001 Vision Node printed housing (ADR 0020): Pi Zero 2 W + PoE HAT + Camera Module 3 Wide NoIR.

    ~/.cache/talos-tools/venv-cad/bin/python hardware/mechanical/vision_node/housing.py [--check]

Writes STEP and STL for each part into hardware/mechanical/vision_node/out/. With --check it builds the
parts, verifies the fit rules and prints the thermal estimate, but writes nothing.

EVERY DIMENSION OF A BOUGHT PART BELOW IS PROVISIONAL. They come from vendor drawings or are typical
values. Measure the parts that arrive (bench VB-01) and fix them here before printing ASA.

How it goes together (README.md):
- body: one print, with the front face down on the bed. The front wall carries three windows: the
  lens in the middle and an 850 nm IR board on each side, each behind its own 30 x 2 mm acrylic disc
  set in a recess (silicone bead). Inside, a ring around the lens window takes a closed-cell foam
  strip, so IR from the boards can't reflect off the disc into the lens (night glare, VB-04).
- camera: Camera Module 3 on four M2 bosses, lens centred in its window. The IR boards sit on their
  own M2 posts.
- Pi stack: the Zero 2 W on the Waveshare PoE HAT, on four M2.5 heat-set inserts in the floor at the
  back. A 50-100 mm Zero camera cable runs to the camera.
- lid (back): four M3 screws into corner posts, with a TPU gasket in a channel. A printed finger on
  the lid presses the lid microswitch (GPIO17), so opening the lid raises tamper.enclosure.
- cable: PoE Ethernet enters through a PG9 gland in the floor near the back, with a drip loop
  outside. There is room inside for the RJ45 plug and its bend.
- mount: a tab on the top of the body, on an M5 bolt and wing nut (tilt), into a clevis on a
  wall/eave plate. The plate's four slotted holes allow pan before the screws are tightened.
Sheltered outdoor only (splash-resistant, under an eave): ASA, no vents (thermal estimate below).
"""

from __future__ import annotations

import math
import pathlib
import sys

import cadquery as cq

HERE = pathlib.Path(__file__).resolve().parent
OUT = HERE / "out"

# --- bought parts (PROVISIONAL: vendor drawings / typical, measure on arrival) ------------------
ZERO = (65.0, 30.0)             # Raspberry Pi Zero 2 W board, mm
ZERO_HOLES = 3.5                # holes this far in from each corner, M2.5 (58 x 23 pattern)
STACK_H = 30.0                  # HAT + standoffs + Zero + heatsink, floor to top (EST)
CAM = (25.0, 24.0)              # Camera Module 3 board
CAM_HOLES = (21.0, 12.5)        # M2 hole pattern; top pair 2 mm below the top edge
CAM_HOLE_TOP = 2.0
CAM_LENS_FROM_TOP = 9.5         # lens centre below the board's top edge (EST)
CAM_DEPTH = 12.4                # board + lens stack, front to back (Wide lens, EST)
IR_BOARD = 30.0                 # one 850 nm board, about 30 x 30 mm (typical "3 W" pair)
IR_HOLES = 22.0                 # its two M2 mounting holes, this far apart, horizontal (EST)
DISC = (30.0, 2.0)              # acrylic window disc, diameter x thickness
SWITCH = (20.0, 10.0, 6.5)      # lever microswitch body (SS-5GL class), with M2 holes 9.5 apart
SWITCH_TRAVEL = 1.0             # lever travel to actuate (EST)
PG9_HOLE = 15.2                 # PG9 cable gland thread
RJ45_BEND = 28.0                # room behind the gland for the plug and the cable bend

# --- design numbers (mm) ------------------------------------------------------------------------
WALL = 2.6
FRONT = 3.0                     # front wall; the discs sit in recesses in it
INNER = (98.0, 74.0, 50.0)      # cavity: width (x), depth front-to-back (y), height (z)
LENS_Z = 31.0                   # lens centre above the cavity floor
IR_OFFSET = 33.0                # IR window centres this far left/right of the lens
LENS_WIN = 18.0                 # clear aperture in front of the lens (checked against the 120 deg view)
IR_WIN = 22.0
RECESS = 1.2                    # disc recess depth (the disc stands 0.8 mm proud for the bead)
BAFFLE = (21.0, 7.0, 1.6)       # foam ring around the lens window: inner diameter, depth, rib
CORNER = 7.0                    # lid screw posts
M3_PILOT, M3_CLEAR = 2.6, 3.4
M2_PILOT = 1.7
INSERT_M25 = 3.6
GASKET = (1.6, 1.0)             # TPU gasket channel in the lid: width, depth
LID_T = 3.0
FINGER_PRELOAD = 0.6            # finger tip this far past the switch's free lever position
TAB = (20.0, 14.0, 6.0)         # mount tab on top: length (y), height (z), thickness (x)
M5 = 5.4
PLATE = (60.0, 60.0, 5.0)
BED = 256.0                     # Bambu P2S

# Thermal (sheltered outdoor, still air): estimate only, VB-01 measures it.
POWER_W = {"day, streaming": 2.2, "night, IR on": 4.2}
H_CONV = 6.0                    # W/m2K natural convection + radiation, small box (EST)
K_ASA = 0.17                    # W/mK
SOC_RISE_K = 18.0               # SoC above internal air with the heatsink at ~1.5 W (EST)
AMBIENT_C = 35.0                # hot day under an eave
THROTTLE_C = 80.0               # Pi firmware soft limit


def outer():
    w, d, h = INNER
    return w + 2 * WALL, d + FRONT + LID_T, h + 2 * WALL


def lens_x():
    return INNER[0] / 2


# Coordinates: cavity corner at the origin; x across, y from the front wall's inner face (y=0)
# toward the lid, z up from the cavity floor.


def body():
    w, d, h = INNER
    ow = w + 2 * WALL
    shell = (cq.Workplane("XY").box(ow, d + FRONT, h + 2 * WALL, centered=False)
             .translate((-WALL, -FRONT, -WALL)))
    shell = shell.cut(cq.Workplane("XY").box(w, d + 1, h, centered=False))
    # Windows: aperture through the front wall plus a disc recess on the outside.
    for x, ap in ((lens_x(), LENS_WIN), (lens_x() - IR_OFFSET, IR_WIN), (lens_x() + IR_OFFSET, IR_WIN)):
        shell = shell.cut(cq.Workplane("XZ").center(x, LENS_Z).circle(ap / 2).extrude(FRONT + 1)
                          .translate((0, 0.5, 0)))
        shell = shell.cut(cq.Workplane("XZ").center(x, LENS_Z).circle(DISC[0] / 2 + 0.3).extrude(-RECESS)
                          .translate((0, -FRONT, 0)))
    # Foam baffle ring around the lens window, inside.
    ring = (cq.Workplane("XZ").center(lens_x(), LENS_Z).circle(BAFFLE[0] / 2 + BAFFLE[2])
            .circle(BAFFLE[0] / 2).extrude(-BAFFLE[1]))
    shell = shell.union(ring)
    # Camera bosses: the board's back face sits CAM_DEPTH behind the front wall.
    cam_top_z = LENS_Z + CAM_LENS_FROM_TOP
    for dx in (-CAM_HOLES[0] / 2, CAM_HOLES[0] / 2):
        for dz in (-CAM_HOLE_TOP, -CAM_HOLE_TOP - CAM_HOLES[1]):
            boss = (cq.Workplane("XZ").center(lens_x() + dx, cam_top_z + dz).circle(2.3)
                    .circle(M2_PILOT / 2).extrude(-(CAM_DEPTH - 1.6)))
            shell = shell.union(boss)
    # IR board posts (two M2 each), 4 mm proud of the wall.
    for side in (-1, 1):
        for dx in (-IR_HOLES / 2, IR_HOLES / 2):
            post = (cq.Workplane("XZ").center(lens_x() + side * IR_OFFSET + dx, LENS_Z).circle(2.3)
                    .circle(M2_PILOT / 2).extrude(-4.0))
            shell = shell.union(post)
    # Pi stack: four insert bosses on the floor at the back, board long side along x.
    for x, y in zero_holes():
        shell = shell.union(cq.Workplane("XY").center(x, y).circle(3.2).circle(INSERT_M25 / 2).extrude(4.0))
    # Lid screw posts in the four back corners (full height).
    for x in (0, w - CORNER):
        for z in (0, h - CORNER):
            post = (cq.Workplane("XZ").center(x + CORNER / 2, z + CORNER / 2).rect(CORNER, CORNER)
                    .extrude(-12.0).translate((0, d - 12.0, 0)))
            shell = shell.union(post).cut(cq.Workplane("XZ").center(x + CORNER / 2, z + CORNER / 2)
                                          .circle(M3_PILOT / 2).extrude(12.5).translate((0, d + 0.5, 0)))
    # Lid microswitch boss: switch body flat on the floor, lever toward the lid.
    sx, sy = switch_xy()
    shell = shell.union(cq.Workplane("XY").center(sx, sy).rect(SWITCH[0] + 4, SWITCH[1] + 4)
                        .rect(SWITCH[0] + 0.4, SWITCH[1] + 0.4).extrude(3.0))
    # PG9 gland in the floor, near the back.
    gx, gy = gland_xy()
    shell = shell.cut(cq.Workplane("XY").center(gx, gy).circle(PG9_HOLE / 2).extrude(-(WALL + 1))
                      .translate((0, 0, 0.5)))
    # Mount tab on top.
    tab = (cq.Workplane("XY").box(TAB[2], TAB[0], TAB[1], centered=(True, True, False))
           .translate((lens_x(), d / 2, h + WALL)))
    tab = tab.cut(cq.Workplane("YZ").center(d / 2, h + WALL + TAB[1] - 5.5).circle(M5 / 2)
                  .extrude(TAB[2] + 2, both=True).translate((lens_x(), 0, 0)))
    return shell.union(tab)


STACK_BACK_GAP = 26.0           # floor behind the stack: the gland and the lid switch live here


def zero_holes():
    # Board centred in x, leaving STACK_BACK_GAP of floor between it and the lid.
    w, d, _ = INNER
    x0 = (w - ZERO[0]) / 2
    y0 = d - STACK_BACK_GAP - ZERO[1]
    return [(x0 + ZERO_HOLES + i * (ZERO[0] - 2 * ZERO_HOLES), y0 + ZERO_HOLES + j * (ZERO[1] - 2 * ZERO_HOLES))
            for i in (0, 1) for j in (0, 1)]


def switch_xy():
    # Behind the stack, right of centre; its lever reaches toward the lid.
    w, d, _ = INNER
    return w / 2 + 20.0, d - 3.0 - SWITCH[1] / 2


def gland_xy():
    # Behind the stack, left of centre, clear of the corner posts.
    w, d, _ = INNER
    return w / 2 - 22.0, d - STACK_BACK_GAP / 2


def rect_overlap(a, b, margin=1.0):
    """Axis-aligned overlap of two (x0, y0, x1, y1) footprints, with a margin."""
    return not (a[2] + margin <= b[0] or b[2] + margin <= a[0] or a[3] + margin <= b[1] or b[3] + margin <= a[1])


def lid():
    w, d, h = INNER
    ow, oh = w + 2 * WALL, h + 2 * WALL
    plate = cq.Workplane("XZ").center(w / 2, h / 2).rect(ow, oh).extrude(-LID_T).translate((0, d, 0))
    for x in (0, w - CORNER):
        for z in (0, h - CORNER):
            plate = plate.cut(cq.Workplane("XZ").center(x + CORNER / 2, z + CORNER / 2).circle(M3_CLEAR / 2)
                              .extrude(-(LID_T + 1)).translate((0, d - 0.5, 0)))
    # Gasket channel on the inner face, just inside the outer edge.
    channel = (cq.Workplane("XZ").center(w / 2, h / 2).rect(w + WALL, h + WALL)
               .rect(w + WALL - 2 * GASKET[0], h + WALL - 2 * GASKET[0]).extrude(-GASKET[1])
               .translate((0, d, 0)))
    plate = plate.cut(channel)
    # Finger to the switch lever: from the inner face toward the front.
    sx, sy = switch_xy()
    reach = d - (sy + SWITCH[1] / 2) + FINGER_PRELOAD
    finger = (cq.Workplane("XY").box(4.0, reach, 3.0, centered=(True, False, False))
              .translate((sx + SWITCH[0] / 2 - 3.0, d - reach, SWITCH[2] - 1.5)))
    return plate.union(finger)


def plate():
    pw, ph, pt = PLATE
    p = cq.Workplane("XY").box(pw, ph, pt, centered=(True, True, False))
    for x in (-20, 20):
        for y in (-20, 20):
            p = p.cut(cq.Workplane("XY").center(x, y).slot2D(9.0, 4.4, angle=90 if x > 0 else 0).extrude(pt + 1))
    ear_gap = TAB[2] + 0.6
    for side in (-1, 1):
        ear = (cq.Workplane("XY").box(4.0, 22.0, 24.0, centered=(True, True, False))
               .translate((side * (ear_gap / 2 + 2.0), 0, pt)))
        ear = ear.cut(cq.Workplane("YZ").center(0, pt + 17.0).circle(M5 / 2).extrude(10, both=True)
                      .translate((side * (ear_gap / 2 + 2.0), 0, 0)))
        p = p.union(ear)
    return p


def thermal():
    ow, od, oh = (v / 1000 for v in outer())
    area = 2 * (ow * od + ow * oh + od * oh)
    rows = {}
    for mode, p in POWER_W.items():
        air = p / (H_CONV * area) + p * (WALL / 1000) / (K_ASA * area)
        soc = AMBIENT_C + air + SOC_RISE_K
        rows[mode] = (air, soc)
    return area, rows


def checks() -> list[str]:
    errs = []
    w, d, h = INNER
    for name, size in (("body", outer()), ("plate", PLATE)):
        if max(size[0], size[1]) > BED:
            errs.append(f"{name} larger than the bed")
    # Windows must not overlap, and the discs must fit on the face.
    gap = IR_OFFSET - DISC[0]
    if gap < 1.5:
        errs.append(f"window discs overlap or touch: {gap:.1f} mm between them")
    if lens_x() + IR_OFFSET + DISC[0] / 2 + 0.3 > w + WALL - 1.0:
        errs.append("IR disc runs off the face")
    if LENS_Z + DISC[0] / 2 > h - 1.0 or LENS_Z - DISC[0] / 2 < 1.0:
        errs.append("window discs run past the top or floor")
    # Camera clear of the IR posts; Pi stack clear of the camera and the lid posts.
    if CAM[0] / 2 + 1.0 > IR_OFFSET - IR_HOLES / 2 - 2.3:
        errs.append("camera board hits the IR posts")
    hx, hy = zero_holes()[0]
    stack = (hx - ZERO_HOLES, hy - ZERO_HOLES, hx - ZERO_HOLES + ZERO[0], hy - ZERO_HOLES + ZERO[1])
    if stack[1] < CAM_DEPTH + 4.0:
        errs.append(f"Pi stack too close to the camera: {stack[1]:.1f} mm from the front wall")
    if STACK_H > h - 4.0:
        errs.append("Pi stack taller than the cavity")
    gx, gy = gland_xy()
    gland = (gx - PG9_HOLE / 2 - 3, gy - PG9_HOLE / 2 - 3, gx + PG9_HOLE / 2 + 3, gy + PG9_HOLE / 2 + 3)
    sx, sy = switch_xy()
    switch = (sx - SWITCH[0] / 2 - 2, sy - SWITCH[1] / 2 - 2, sx + SWITCH[0] / 2 + 2, sy + SWITCH[1] / 2 + 2)
    posts = [(x, d - 12.0, x + CORNER, d) for x in (0.0, w - CORNER)]
    for name, fp in (("gland", gland), ("lid switch", switch)):
        if rect_overlap(fp, stack):
            errs.append(f"{name} under the Pi stack")
        if any(rect_overlap(fp, p, 0.0) for p in posts) or fp[0] < 0 or fp[2] > w or fp[3] > d:
            errs.append(f"{name} hits a lid post or the wall")
    if rect_overlap(gland, switch):
        errs.append("gland and lid switch overlap")
    if not 0.3 <= FINGER_PRELOAD <= SWITCH_TRAVEL:
        errs.append("finger preload outside the switch travel")
    # Lens field: a 120 deg diagonal (about 102 x 67 deg) must clear the aperture edge.
    half = math.radians(102 / 2)
    need = 2 * (CAM_DEPTH - 8.0) * math.tan(half) + 6.0
    if LENS_WIN < need:
        errs.append(f"lens window {LENS_WIN} mm vignettes the 120 deg view (needs {need:.1f})")
    return errs


def solid_probes(body_s, lid_s) -> list[str]:
    """Points that must be solid or open: catch a feature cut or placed the wrong way."""
    w, d, h = INNER
    cam_hole = (lens_x() + CAM_HOLES[0] / 2, 5.0, LENS_Z + CAM_LENS_FROM_TOP - CAM_HOLE_TOP)
    want = [
        ("top wall", body_s, (w / 2 + 30, d / 2, h + 1.3), True),
        ("front wall", body_s, (10, -1.5, 10), True),
        ("lens window", body_s, (lens_x(), -1.5, LENS_Z), False),
        ("IR window", body_s, (lens_x() + IR_OFFSET, -1.5, LENS_Z), False),
        ("disc recess", body_s, (lens_x() + 12, -FRONT + RECESS / 2, LENS_Z), False),
        ("wall behind the recess", body_s, (lens_x() + 12, -1.0, LENS_Z), True),
        ("cavity", body_s, (w / 2, d / 2, h / 2), False),
        ("lid post", body_s, (1.0, d - 6, 1.0), True),
        ("lid post pilot", body_s, (CORNER / 2, d - 3, CORNER / 2), False),
        ("camera boss pilot", body_s, cam_hole, False),
        ("gland hole", body_s, (*gland_xy(), -1.3), False),
        ("lid plate", lid_s, (w / 2, d + 1.5, h / 2), True),
        ("lid screw hole", lid_s, (CORNER / 2, d + 1.5, CORNER / 2), False),
    ]
    errs = []
    for name, shape, pt, solid in want:
        if shape.val().isInside(cq.Vector(*pt)) != solid:
            errs.append(f"{name} should be {'solid' if solid else 'open'} at {pt}")
    return errs


def main(argv: list[str]) -> int:
    parts = {"body": body(), "lid": lid(), "mount-plate": plate()}
    errs = checks() + solid_probes(parts["body"], parts["lid"])
    area, rows = thermal()
    print(f"TL-VN-001 housing {outer()[0]:.1f} x {outer()[1]:.1f} x {outer()[2]:.1f} mm (w x d x h), "
          f"outer area {area * 1e4:.0f} cm2")
    for mode, (air, soc) in rows.items():
        flag = "OK" if soc < THROTTLE_C else "OVER"
        print(f"  thermal ({mode}, {AMBIENT_C:.0f} C ambient): air +{air:.1f} K, SoC ~{soc:.0f} C "
              f"(limit {THROTTLE_C:.0f}): {flag}  [estimate; VB-01 measures it]")
    for e in errs:
        print("FIT ERROR:", e)
    if "--check" in argv:
        return 1 if errs else 0
    OUT.mkdir(exist_ok=True)
    for name, shape in parts.items():
        cq.exporters.export(shape, str(OUT / f"vn-{name}.step"))
        cq.exporters.export(shape, str(OUT / f"vn-{name}.stl"), tolerance=0.05, angularTolerance=0.2)
    print(f"wrote {len(parts)} parts to {OUT.relative_to(HERE.parents[2])}")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
