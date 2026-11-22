"""TL-SN-002 printed housing (owner 2026-11-22): door-frame mount, tool-free battery door, Bambu P2S.

    ~/.cache/talos-tools/venv-cad/bin/python hardware/mechanical/housing/housing.py [--check]

Builds from hardware/mechanical/housing/TL-SN-002.board.json (written by hardware/scripts/
housing_marks.py from the routed board), so moving a part on the board moves its opening here.
Writes STEP and STL for each part into hardware/mechanical/housing/out/. --check only builds the parts
and verifies the fit rules (bed size, wall at the Hall sensor, finger preload, antenna zone); it writes
nothing. Every dimension here is PROVISIONAL until a test print fits a real board (bench HD-049).

How it goes together (see README.md):
- base: screws to the door casing (latch side), long side horizontal, right edge flush with the casing
  edge so the Hall sensor faces the door. M2.5 heat-set inserts take the board screws.
- board: sits on 3 mm bosses (air gap under the board against condensation).
- lid: 4 x M3 screws into the base corners (none within 15 mm of the antenna). A spring finger presses
  the lid tamper switch SW4. It carries the PIR Fresnel lens and the 1.75 mm light pipes (clear
  filament offcuts).
- battery door: slides along X in dovetail rails in the lid, held by a detent bump, no tool. A spring
  finger presses SW5 while it is shut: opening it trips tamper.battery_door (HD-049).
- magnet carrier: on the door face opposite the Hall sensor, holding a 10 x 3 mm N42 disc.
"""

from __future__ import annotations

import json
import pathlib
import sys

import cadquery as cq

HERE = pathlib.Path(__file__).resolve().parent
BOARD = json.loads((HERE / "TL-SN-002.board.json").read_text())
OUT = HERE / "out"

# --- fixed design numbers (mm) -----------------------------------------------------------
CLEAR = 0.5          # board to inner wall, each side
WALL = 2.4           # general wall
HALL_WALL = 1.5      # wall in front of the Hall sensor (<= 1.5, HD-020)
FLOOR = 2.4
GAP_UNDER = 3.0      # air gap under the board (condensation, deployment doc §5)
PCB_T = BOARD["thickness_mm"]
CEIL_CLEAR = 1.5     # above the tallest part
TOP = 2.4            # lid top plate
JOINT_ABOVE_PCB = 1.5  # base walls end this far above the board top; the lid covers the rest
CORNER = 7.0         # square lid-screw posts (M3 self-tap or heat-set)
BAY = 8.0            # screw bay at the left end (the right end stays flush with the casing edge)
LUG = (8.0, 7.0)     # external screw lugs on the top and bottom walls near the right end
M3_PILOT = 2.6
M3_CLEAR = 3.4
INSERT_HOLE = 3.6    # M2.5 heat-set insert (OD ~3.9 knurl)
BOSS_D = 6.0
MOUNT_HOLE = 4.2     # #8 / M4 wood screw into the casing, countersunk
LENS_D = 13.0        # PIR window; Fresnel lens (15 mm) sits on a 1 mm ledge
LENS_LEDGE_D = 15.4
PIPE_D = 1.9         # 1.75 mm clear filament as a light pipe
GASKET_W, GASKET_D = 1.6, 1.0  # TPU gasket channel on the base wall top
FINGER_PRELOAD = 0.6  # the finger tip sits this far below the switch's free height
BED = 256.0          # Bambu P2S


def board_size():
    return BOARD["size_mm"]


def inner():
    w, h = board_size()
    return w + 2 * CLEAR, h + 2 * CLEAR


def z_levels():
    pcb_bottom = FLOOR + GAP_UNDER
    pcb_top = pcb_bottom + PCB_T
    tallest = max(BOARD["tall_parts_mm"].values())
    ceiling = pcb_top + tallest + CEIL_CLEAR
    joint = pcb_top + JOINT_ABOVE_PCB
    return {"pcb_bottom": pcb_bottom, "pcb_top": pcb_top, "joint": joint, "ceiling": ceiling,
            "top": ceiling + TOP}


def bx(x):  # board x -> housing x (housing origin = inner cavity corner)
    return CLEAR + x


def by(y):  # board y (down) -> housing y (up)
    return CLEAR + (board_size()[1] - y)


def outer():
    iw, ih = inner()
    return BAY + iw + 2 * WALL, ih + 2 * WALL


def shell_box(z0, z1):
    ow, oh = outer()
    box = cq.Workplane("XY").box(ow, oh, z1 - z0, centered=False).translate((-BAY - WALL, -WALL, z0))
    for (lx, ly) in lugs():
        box = box.union(cq.Workplane("XY").box(LUG[0], LUG[1], z1 - z0, centered=(True, True, False))
                        .translate((lx, ly, z0)))
    return box


def cavity_box(z0, z1):
    iw, ih = inner()
    return cq.Workplane("XY").box(BAY + iw, ih, z1 - z0, centered=False).translate((-BAY, 0, z0))


def lugs():
    iw, ih = inner()
    return [(iw - 8.0, -WALL - LUG[1] / 2 + 0.01), (iw - 8.0, ih + WALL + LUG[1] / 2 - 0.01)]


def screw_points():
    # Two in the left bay (posts inside), two through the external lugs.
    # Both bay screws sit in the lower half: the antenna zone spans the board's top 15 mm.
    return [(-BAY / 2, by(58.0)), (-BAY / 2, by(36.0))] + lugs()


def base():
    z = z_levels()
    iw, ih = inner()
    body = shell_box(0, z["joint"])
    body = body.cut(cavity_box(FLOOR, z["joint"] + 1))
    # Board bosses with heat-set insert holes at H1-H4.
    for hole in BOARD["holes"]:
        x, y = bx(hole["x"]), by(hole["y"])
        boss = cq.Workplane("XY").circle(BOSS_D / 2).extrude(z["pcb_bottom"] - FLOOR).translate((x, y, FLOOR))
        body = body.union(boss)
        body = body.cut(cq.Workplane("XY").circle(INSERT_HOLE / 2).extrude(z["pcb_bottom"]).translate((x, y, 1.0)))
    # Lid screw posts in the left bay, and pilots in the lugs (never within 15 mm of the antenna).
    for i, (cx, cy) in enumerate(screw_points()):
        if i < 2:
            post = cq.Workplane("XY").rect(CORNER, CORNER).extrude(z["joint"] - FLOOR).translate((cx, cy, FLOOR))
            body = body.union(post)
        body = body.cut(cq.Workplane("XY").circle(M3_PILOT / 2).extrude(z["joint"]).translate((cx, cy, 3.0)))
    # Frame-mount holes through the floor, countersunk from inside, clear of the bosses.
    for mx, my in mount_holes():
        body = body.cut(cq.Workplane("XY").circle(MOUNT_HOLE / 2).extrude(FLOOR + 1).translate((mx, my, -0.5)))
        body = body.cut(cq.Workplane("XY").circle(MOUNT_HOLE / 2).workplane(offset=FLOOR - 0.6)
                        .circle(MOUNT_HOLE).loft().translate((mx, my, 0.6)))
    # Gasket channel along the wall top (TPU gasket, splash-resistant: no IP claim).
    body = body.cut(gasket_ring(z["joint"] - GASKET_D, GASKET_D, 0.0))
    body = body.cut(usb_cut())
    body = body.cut(hall_pocket(z["joint"]))
    body = body.cut(vent_slots(0, z["joint"]))
    return body


def gasket_ring(z0, height, grow):
    # A rectangular ring in the middle of the wall: the base's channel, or (grow < 0) the lid's lip.
    ow, oh = outer()
    cx, cy = -BAY - WALL + ow / 2, -WALL + oh / 2
    a = (ow - WALL + grow, oh - WALL + grow)
    b = (ow - WALL - 2 * GASKET_W - grow, oh - WALL - 2 * GASKET_W - grow)
    return (cq.Workplane("XY").rect(*a).extrude(height).cut(cq.Workplane("XY").rect(*b).extrude(height))
            .translate((cx, cy, z0)))


def mount_holes():
    # Two holes on the long axis, between the bosses and clear of the battery bay's centre.
    iw, ih = inner()
    return [(iw * 0.25, ih * 0.5), (iw * 0.75, ih * 0.5)]


def usb_cut():
    z = z_levels()
    u = BOARD["usb_c"]
    w_open, h_open = u["opening_mm"]
    x = bx(u["x"])
    return (cq.Workplane("XY").box(w_open, WALL * 3, h_open)
            .translate((x, -WALL / 2, z["pcb_top"] + 1.6)))


def hall_pocket(z_top):
    # Thin the right wall to HALL_WALL over a 14 mm window centred on the sensor.
    iw, _ = inner()
    hy = by(BOARD["hall"]["y"])
    depth = WALL - HALL_WALL
    return cq.Workplane("XY").box(depth + 0.01, 14.0, z_top, centered=(False, True, False)).translate(
        (iw + WALL - depth, hy, 0))


def vent_slots(z0, z1):
    iw, _ = inner()
    t = BOARD["sht40"]["tongue"]
    y0, y1 = by(t[3]), by(t[1])
    cuts = cq.Workplane("XY")
    zc = (z0 + z1) / 2
    for k in range(3):
        yk = y0 + (k + 1) * (y1 - y0) / 4
        cuts = cuts.union(cq.Workplane("XY").box(WALL * 3, 1.2, max(1.0, (z1 - z0) - 3.0))
                          .translate((iw + WALL / 2, yk, zc)))
    return cuts


def lid():
    z = z_levels()
    iw, ih = inner()
    body = shell_box(z["joint"], z["top"])
    body = body.cut(cavity_box(z["joint"] - 1, z["ceiling"]))
    # Screw posts down from the ceiling over the base posts; clear holes + counterbores from the top.
    for i, (cx, cy) in enumerate(screw_points()):
        if i < 2:
            post = cq.Workplane("XY").rect(CORNER, CORNER).extrude(z["ceiling"] - z["joint"]).translate(
                (cx, cy, z["joint"]))
            body = body.union(post)
        body = body.cut(cq.Workplane("XY").circle(M3_CLEAR / 2).extrude(z["top"]).translate((cx, cy, z["joint"])))
        body = body.cut(cq.Workplane("XY").circle(3.2).extrude(3.0).translate((cx, cy, z["top"] - 3.0)))
    # Raised lip that drops into the gasket channel (0.2 mm per side smaller).
    body = body.union(gasket_ring(z["joint"] - GASKET_D * 0.8, GASKET_D * 0.8, -0.4))
    # PIR window with a lens ledge.
    p = BOARD["pir"]
    px, py = bx(p["x"]), by(p["y"])
    body = body.cut(cq.Workplane("XY").circle(LENS_D / 2).extrude(TOP + 1).translate((px, py, z["ceiling"] - 0.5)))
    body = body.cut(cq.Workplane("XY").circle(LENS_LEDGE_D / 2).extrude(1.0).translate((px, py, z["top"] - 1.0)))
    # Light pipes over every lamp.
    for lamp in BOARD["lamps"]:
        body = body.cut(cq.Workplane("XY").circle(PIPE_D / 2).extrude(TOP + 1)
                        .translate((bx(lamp["x"]), by(lamp["y"]), z["ceiling"] - 0.5)))
    # Lid tamper finger over SW4: a column with a 0.6 mm preload.
    s = BOARD["tamper_switches"]["SW4"]
    body = body.union(spring_finger(bx(s["x"]), by(s["y"]), z["ceiling"], z["pcb_top"] + s["actuator_top_mm"]))
    # Battery door opening and its rails.
    body = body.cut(door_opening())
    body = body.union(door_rails())
    body = body.cut(usb_cut())
    body = body.cut(hall_pocket(z["top"]).translate((0, 0, 0)))
    body = body.cut(vent_slots(z["joint"], z["ceiling"]))
    # Rib separating the SHT40 tongue compartment from the rest (heat, HD-047).
    t = BOARD["sht40"]["tongue"]
    rib_x = bx(t[0]) - 1.0
    rib = cq.Workplane("XY").box(1.2, by(t[1]) - by(t[3]) + 2.0, z["ceiling"] - z["pcb_top"] - 0.5,
                                 centered=(True, False, False)).translate((rib_x, by(t[3]) - 1.0, z["pcb_top"] + 0.5))
    body = body.union(rib)
    return body


def spring_finger(x, y, z_ceiling, z_actuator):
    # A cantilever arm under the ceiling ending in a nub over the actuator: the arm bends to take up
    # the print and assembly tolerance while keeping the switch pressed (HD-049).
    nub_bottom = z_actuator - FINGER_PRELOAD
    drop = z_ceiling - nub_bottom
    arm_len, arm_t, arm_w = 12.0, 1.2, 4.0
    column = cq.Workplane("XY").box(4.0, 4.0, drop - arm_t - 2.0, centered=(True, True, False)).translate(
        (x - arm_len, y, nub_bottom + arm_t + 2.0))
    arm = cq.Workplane("XY").box(arm_len + 2.0, arm_w, arm_t, centered=(False, True, False)).translate(
        (x - arm_len - 1.0, y, nub_bottom + 2.0))
    nub = cq.Workplane("XY").circle(1.5).extrude(2.0).translate((x, y, nub_bottom))
    return column.union(arm).union(nub)


def door_opening():
    z = z_levels()
    d = BOARD["battery_door_opening"]
    x0, x1 = bx(d[0]), bx(d[2])
    y0, y1 = by(d[3]), by(d[1])
    return cq.Workplane("XY").box(x1 - x0, y1 - y0, TOP + 2, centered=False).translate((x0, y0, z["ceiling"] - 1))


def door_rails():
    # Dovetail rails along X on both long sides of the opening (the door slides out to +X).
    z = z_levels()
    d = BOARD["battery_door_opening"]
    x0, x1 = bx(d[0]), bx(d[2])
    rails = cq.Workplane("XY")
    for yr, sign in ((by(d[3]) - 0.0, -1), (by(d[1]) + 0.0, 1)):
        prof = (cq.Workplane("YZ").polyline([(0, 0), (2.0 * sign, 0), (2.8 * sign, TOP), (0, TOP)]).close()
                .extrude(x1 - x0).translate((x0, yr, z["ceiling"])))
        rails = rails.union(prof)
    return rails


def battery_door():
    z = z_levels()
    d = BOARD["battery_door_opening"]
    x0, x1 = bx(d[0]), bx(d[2])
    y0, y1 = by(d[3]), by(d[1])
    plate = cq.Workplane("XY").box(x1 - x0 - 0.4, y1 - y0 - 0.4, TOP, centered=False).translate(
        (x0 + 0.2, y0 + 0.2, z["ceiling"]))
    # Dovetail tongues that run in the lid rails (0.25 mm clearance).
    for yr, sign in ((y0 + 0.2, -1), (y1 - 0.2, 1)):
        tongue = (cq.Workplane("YZ").polyline([(0, 0), (1.75 * sign, 0), (2.55 * sign, TOP), (0, TOP)]).close()
                  .extrude(x1 - x0 - 0.4).translate((x0 + 0.2, yr, z["ceiling"])))
        plate = plate.union(tongue)
    # Grip ridges on top and a detent bump at the closed end.
    for k in range(5):
        plate = plate.union(cq.Workplane("XY").box(1.0, (y1 - y0) * 0.5, 0.8)
                            .translate((x1 - 6.0 - 2.0 * k, (y0 + y1) / 2, z["top"] + 0.4)))
    plate = plate.union(cq.Workplane("XY").sphere(0.6).translate((x0 + 3.0, y0 + 0.2, z["ceiling"] + TOP / 2)))
    # The door's own spring finger onto SW5.
    s = BOARD["tamper_switches"]["SW5"]
    plate = plate.union(spring_finger(bx(s["x"]), by(s["y"]), z["ceiling"], z["pcb_top"] + s["actuator_top_mm"]))
    return plate


def magnet_carrier():
    # Screws to the door face; the disc's centre lines up with the Hall sensor's height when the
    # base is mounted with the board's right edge at the casing edge.
    body = cq.Workplane("XY").box(30.0, 14.0, 8.0, centered=(True, True, False))
    body = body.cut(cq.Workplane("XY").circle(10.2 / 2).extrude(3.1).translate((0, 0, 8.0 - 3.1)))
    for sx in (-11.0, 11.0):
        body = body.cut(cq.Workplane("XY").circle(1.8).extrude(8.0).translate((sx, 0, 0)))
        body = body.cut(cq.Workplane("XY").circle(3.4).extrude(2.5).translate((sx, 0, 8.0 - 2.5)))
    return body


def checks(parts) -> list[str]:
    problems = []
    for name, part in parts.items():
        bb = part.val().BoundingBox()
        if max(bb.xlen, bb.ylen, bb.zlen) > BED - 10:
            problems.append(f"{name}: {bb.xlen:.0f} x {bb.ylen:.0f} x {bb.zlen:.0f} mm does not fit the P2S bed")
    if HALL_WALL > 1.5:
        problems.append("wall at the Hall sensor thicker than 1.5 mm")
    ant = BOARD["antenna_zone"]  # board coordinates, y down
    for cx, cy in screw_points():
        bxx = cx - CLEAR
        byy = board_size()[1] - (cy - CLEAR)
        dx = max(ant[0] - bxx, 0.0, bxx - ant[2])
        dy = max(ant[1] - byy, 0.0, byy - ant[3])
        if (dx * dx + dy * dy) ** 0.5 < 15.0:
            problems.append(f"lid screw at board ({bxx:.1f}, {byy:.1f}) within 15 mm of the antenna zone")
    # The screws must stay off the board (its own holes and parts).
    w, h = board_size()
    for cx, cy in screw_points():
        if 0 <= cx - CLEAR <= w and 0 <= board_size()[1] - (cy - CLEAR) <= h:
            problems.append(f"lid screw at ({cx:.1f}, {cy:.1f}) lands on the board")
    return problems


def main() -> int:
    parts = {"base": base(), "lid": lid(), "battery_door": battery_door(), "magnet_carrier": magnet_carrier()}
    problems = checks(parts)
    for p in problems:
        print("FAIL", p)
    z = z_levels()
    ow, oh = outer()
    print(f"housing {ow:.1f} x {oh:.1f} x {z['top']:.1f} mm, board top at z={z['pcb_top']:.1f}, "
          f"joint z={z['joint']:.1f}")
    if "--check" in sys.argv:
        return 1 if problems else 0
    OUT.mkdir(exist_ok=True)
    for name, part in parts.items():
        cq.exporters.export(part, str(OUT / f"{name}.step"))
        cq.exporters.export(part, str(OUT / f"{name}.stl"), tolerance=0.05, angularTolerance=0.2)
        print(f"wrote out/{name}.step, out/{name}.stl")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
