"""Project-local KiCad footprints (hardware/kicad/libraries/talos-footprints.pretty).

Sources:
- ESP32-H2-MINI-1: Espressif datasheet v1.6 Fig. 11 (pads 0.4 x 0.8 at 0.8 mm, columns 11.8 mm
  apart, rows 9.8 mm, corner pads 0.7 at 11.9 x 9.9, 3x3 1.45 mm EP array over 5.4 mm). That geometry
  is identical to KiCad's ESP32-C6-MINI-1 footprint, which is reused with the H2 description.
- BQ25186 DLH0010A: TI SLUSF69 §12 land pattern example (10 x 0.5 x 0.2 mm at 0.4 mm, 2.1 mm row
  span, EP 0.9 x 1.5 mm), modified: rows at +/-0.825 and EP 0.75 x 1.5 mm for 0.2 mm pad-to-EP copper.
- EKMB: Panasonic PaPIRs WL reference spec rev 7.1: three 0.45 mm leads on a 5.08 mm PCD; GND and OUT
  at +/-45 deg from the bottom vertical, Vdd opposite. Mapping VERIFY against a sample.
- sensor_edge_v1 card fingers: Samtec "recommended mating card layout for HSEC8-1XX-XX-X-D-EMX"
  rev K sheet 3, -10 column: 0.55 mm fingers at 0.80 mm (odd top, even bottom), 2.80 mm contact zone
  ending 1.00 mm from the card edge, 1.57 mm card, 9.25 mm wide mating tongue (no polarising slot
  for -10), 30 deg edge chamfer. The drawing continues the copper to 0.15 mm from the edge as a
  plating tie; Talos stops it 0.65 mm from the edge so the bevel can't grind into it (HD-040).
- HSEC8-110-01-S-D-EM2: Samtec "recommended PCB layout for HSEC8-1XX-XX-X-D-EMX" rev K sheet 1,
  Fig. 1 (-10/-20/-30) and product drawing HSEC8-1XX-XX-X-D-EMXX rev M (-EM2: 1.57 mm host).
  Read from the drawings (kept locally in hardware/vendor-docs/, not committed):
  signal pads 0.50 x 2.00 at 0.80 mm, 0.80-2.80 mm in from the host edge, mirrored on the bottom
  (pin 2 under pin 1); weld-tab pads 1.00 x 2.00, 1.50-3.50 mm in from the edge, 19.70 mm apart
  ("D"), also on both sides; a 1.549 mm (+0.08/-0.03) slot in the host board 3.750 mm outboard of
  pin 1 and 4.375 mm deep with a full-radius end; 5.00 mm deep keep-out rework area; host edge at
  least 23.20 mm long ("E"). The slot depth and the 0.80 mm pad setback are read from the drawing
  geometry, not from a printed callout: VERIFY against a sample socket before fabrication.
"""

from __future__ import annotations

import math
import os
import pathlib

from kicad_sexpr import q

STOCK_FP = os.environ.get(
    "KICAD_FOOTPRINT_DIR",
    os.path.expanduser("~/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"),
)


def _f(v: float) -> str:
    return f"{round(v, 4):g}"


def _text(kind: str, value: str, y: float, layer: str, hide: bool = False) -> str:
    h = " (hide yes)" if hide else ""
    return (f'\t(property "{kind}" {q(value)} (at 0 {_f(y)} 0) (layer "{layer}"){h}\n'
            f'\t\t(effects (font (size 1 1) (thickness 0.15))))\n')


def _rect(x0, y0, x1, y1, layer, width=0.1) -> str:
    return (f'\t(fp_rect (start {_f(x0)} {_f(y0)}) (end {_f(x1)} {_f(y1)}) '
            f'(stroke (width {width}) (type solid)) (fill no) (layer "{layer}"))\n')


def _line(x0, y0, x1, y1, layer, width=0.12) -> str:
    return (f'\t(fp_line (start {_f(x0)} {_f(y0)}) (end {_f(x1)} {_f(y1)}) '
            f'(stroke (width {width}) (type solid)) (layer "{layer}"))\n')


def _smd(num, x, y, w, h, side="F", paste=True, shape="roundrect") -> str:
    layers = f'"{side}.Cu" "{side}.Mask"' + (f' "{side}.Paste"' if paste else "")
    extra = " (roundrect_rratio 0.25)" if shape == "roundrect" else ""
    return f'\t(pad "{num}" smd {shape} (at {_f(x)} {_f(y)}) (size {_f(w)} {_f(h)}) (layers {layers}){extra})\n'


def _tht(num, x, y, size, drill) -> str:
    return (f'\t(pad "{num}" thru_hole circle (at {_f(x)} {_f(y)}) (size {_f(size)} {_f(size)}) '
            f'(drill {_f(drill)}) (layers "*.Cu" "*.Mask"))\n')


def _footprint(name: str, descr: str, attr: str, body: str, ref_y: float, val_y: float) -> str:
    return (f'(footprint {q(name)}\n\t(version 20241229)\n\t(generator "talos_gen")\n\t(layer "F.Cu")\n'
            f'\t(descr {q(descr)})\n\t(attr {attr})\n'
            + _text("Reference", "REF**", ref_y, "F.SilkS") + _text("Value", name, val_y, "F.Fab")
            + body + ")\n")


def dlh0010a() -> str:
    body = ""
    # TI's example (pads at +/-0.8, EP 0.9 wide) leaves 0.1 mm pad-to-EP copper, below the 0.15 mm fab
    # rule, and the autorouter then refuses every pin. Pads move out 0.025 mm and the EP narrows to
    # 0.75 mm, giving 0.2 mm. VERIFY with an EE: EP solder area drops ~17 % (thermal via in the EP).
    for i in range(5):
        y = -0.8 + 0.4 * i
        body += _smd(str(1 + i), -0.825, y, 0.5, 0.2)  # pins 1-5 down the left column
        body += _smd(str(10 - i), 0.825, y, 0.5, 0.2)  # pins 10-6 down the right column
    body += _smd("11", 0, 0, 0.75, 1.5, shape="rect")
    body += _rect(-1.1, -1.0, 1.1, 1.0, "F.Fab")
    body += _rect(-1.35, -1.25, 1.35, 1.25, "F.CrtYd", 0.05)
    body += _line(-1.2, -1.15, -0.6, -1.15, "F.SilkS")  # pin-1 marker
    return _footprint("Texas_DLH0010A_WSON-10-1EP_2.2x2mm_P0.4mm",
                      "TI DLH0010A WSON-10, from SLUSF69 §12 land pattern with EP narrowed to 0.75 mm for 0.2 mm pad-to-EP clearance (BQ25186); VERIFY", "smd", body, -2, 2)


def ekmb() -> str:
    r = 5.08 / 2
    d = r * math.sin(math.radians(45))
    # Top view = bottom view mirrored in X. Bottom view: GND lower-left, OUT lower-right, Vdd top.
    body = _tht("1", 0, -r, 1.4, 0.8)  # Vdd
    body += _tht("2", -d, d, 1.4, 0.8)  # OUT
    body += _tht("3", d, d, 1.4, 0.8)  # GND
    body += f'\t(fp_circle (center 0 0) (end 4.75 0) (stroke (width 0.1) (type solid)) (fill no) (layer "F.Fab"))\n'
    body += f'\t(fp_circle (center 0 0) (end 5.5 0) (stroke (width 0.12) (type solid)) (fill no) (layer "F.SilkS"))\n'
    body += f'\t(fp_circle (center 0 0) (end 5.75 0) (stroke (width 0.05) (type solid)) (fill no) (layer "F.CrtYd"))\n'
    return _footprint("PIR_Panasonic_EKMB_TO-5",
                      "Panasonic PaPIRs EKMB, 3 leads on PCD 5.08 mm, 9.5 mm lens dia; pin mapping VERIFY", "through_hole",
                      body, -6.5, 6.5)


def pir223d() -> str:
    """NEWOPTO XYC-PIR223D-S0 (spec 2020-06-01, p5 figure C): three 0.45 mm leads on a 5.08 mm PCD at
    90 degree steps. Bottom view: pin 1 (VDD) at 12 o'clock, pin 2 (REL) at 3, pin 3 (VSS) at 6, tab at 45
    degrees between pin 1 and 9 o'clock. Top view (this footprint) mirrors X: pin 2 at 9 o'clock, tab
    upper-right. Not the EKMB pattern (HD-044)."""
    r = 5.08 / 2
    body = _tht("1", 0, -r, 1.4, 0.8)  # VDD
    body += _tht("2", -r, 0, 1.4, 0.8)  # REL (output)
    body += _tht("3", 0, r, 1.4, 0.8)  # VSS
    t = 4.55 / math.sqrt(2)
    body += f'\t(fp_circle (center 0 0) (end 4.55 0) (stroke (width 0.1) (type solid)) (fill no) (layer "F.Fab"))\n'
    body += f'\t(fp_line (start {t:.3f} {-t:.3f}) (end {t + 0.57:.3f} {-t - 0.57:.3f}) (stroke (width 0.1) (type solid)) (layer "F.Fab"))\n'
    body += f'\t(fp_circle (center 0 0) (end 5.3 0) (stroke (width 0.12) (type solid)) (fill no) (layer "F.SilkS"))\n'
    body += f'\t(fp_circle (center 0 0) (end 5.6 0) (stroke (width 0.05) (type solid)) (fill no) (layer "F.CrtYd"))\n'
    return _footprint("PIR_NEWOPTO_PIR223D_TO-5",
                      "NEWOPTO XYC-PIR223D-S0 digital PIR, 3 leads 0.45 mm on PCD 5.08 mm at 90 deg, can 9.1 mm",
                      "through_hole", body, -6.4, 6.4)


# Samtec HSEC8-EM2, -10 position (see module docstring). Footprint frame: host edge at y = +1.0,
# host board toward -y, socket body and card toward +y, pin 1 at +x (the drawing's top view turned
# 180 degrees so the edge faces the card).
HSEC8_EDGE_Y = 1.0
HSEC8_PIN1_X = 3.6
HSEC8_PITCH = 0.8
HSEC8_TAB_X = 9.85  # "D" 19.70 / 2
HSEC8_SLOT = {"x": HSEC8_PIN1_X + 3.75, "width": 1.549, "depth": 4.375}
HSEC8_KEEPOUT_DEPTH = 5.0
HSEC8_E_MIN = 23.2
HSEC8_BODY_OUT = 7.8  # body beyond the host edge, card side (REF)
FINGER_PULLBACK_MM = 0.65  # finger copper to card edge (bevel clearance, HD-040)
CARD_TONGUE_WIDTH = 9.25
CARD_TONGUE_DEPTH = 8.5  # > HSEC8_BODY_OUT: the card's shoulders stay clear of the socket body


def card_fingers() -> str:
    body = ""
    # Contact zone 1.0-3.8 mm from the edge. Samtec's drawing runs the copper on to 0.15 mm (plating
    # tie), but a 30 deg bevel then grinds into it (HD-040): the copper stops 0.65 mm from the edge.
    y0, y1 = 1.65 - 3.8, 1.65 - FINGER_PULLBACK_MM
    for i in range(10):
        x = -HSEC8_PIN1_X + HSEC8_PITCH * i
        for num, side in ((2 * i + 1, "F"), (2 * i + 2, "B")):
            body += _smd(str(num), x, (y0 + y1) / 2, 0.55, y1 - y0, side, paste=False, shape="rect")
    body += _rect(-4.625, y0 - 0.3, 4.625, 1.65, "F.CrtYd", 0.05)
    body += _rect(-4.625, y0 - 0.3, 4.625, 1.65, "B.CrtYd", 0.05)
    body += _text("Talos_Note", "Card edge at y=+1.65; 30 deg chamfer; 30 uin hard gold (Samtec HSEC8 mating card)", 3.5, "F.Fab", True)
    return _footprint("CardEdge_sensor_edge_v1_2x10_P0.8mm",
                      "sensor_edge_v1 gold fingers for Samtec HSEC8-110-01-S-D-EM2: 2x10 at 0.80 mm, 0.55 mm wide, "
                      "odd=top even=bottom, 1.57 mm card, 9.25 mm tongue. Card edge at y=+1.65",
                      "smd", body, -3, 3)


def hsec8_em2() -> str:
    e = HSEC8_EDGE_Y
    body = ""
    for i in range(10):
        x = HSEC8_PIN1_X - HSEC8_PITCH * i
        body += _smd(str(2 * i + 1), x, e - 1.8, 0.5, 2.0, "F")
        body += _smd(str(2 * i + 2), x, e - 1.8, 0.5, 2.0, "B")
    for side in ("F", "B"):  # weld tabs: soldered, no net
        for n, x in (("MP1", -HSEC8_TAB_X), ("MP2", HSEC8_TAB_X)):
            body += _smd(n, x, e - 2.5, 1.0, 2.0, side)
    half = HSEC8_E_MIN / 2
    body += _rect(-half, e, half, e + HSEC8_BODY_OUT, "F.Fab")
    body += _line(-half, e, half, e, "F.SilkS")
    # Courtyard = the drawing's keep-out rework area on the host plus the body beyond the edge.
    for layer in ("F.CrtYd", "B.CrtYd"):
        body += _rect(-half, e - HSEC8_KEEPOUT_DEPTH, half, e + HSEC8_BODY_OUT, layer, 0.05)
    sx, sw, sd = HSEC8_SLOT["x"], HSEC8_SLOT["width"], HSEC8_SLOT["depth"]
    body += _rect(sx - sw / 2, e - sd, sx + sw / 2, e, "Dwgs.User", 0.05)
    body += _text("Talos_Note", "Host board needs the 1.549 mm locating slot (cut by gen_pcbs.py); 1.57 mm host", -4.5, "F.Fab", True)
    return _footprint("Samtec_HSEC8-110-01-S-D-EM2",
                      "Samtec HSEC8-110-01-S-D-EM2 edge-mount socket, 2x10 at 0.80 mm, straddling a 1.57 mm host edge "
                      "(host edge at y=+1.0). From Samtec HSEC8-1XX-XX-X-D-EMX recommended PCB layout rev K. "
                      "Slot depth and pad setback read from drawing geometry: VERIFY with a sample (HD-002).",
                      "smd", body, -6, e + HSEC8_BODY_OUT + 1)


def esp32_h2_mini1() -> str:
    src = pathlib.Path(STOCK_FP) / "RF_Module.pretty" / "ESP32-C6-MINI-1.kicad_mod"
    text = src.read_text(encoding="utf-8")
    text = text.replace('(footprint "ESP32-C6-MINI-1"', '(footprint "ESP32-H2-MINI-1"', 1)
    start = text.index("(descr ")
    end = text.index("\n", start)
    text = (text[:start]
            + '(descr "ESP32-H2-MINI-1 (Thread/BLE). Geometry checked against Espressif ESP32-H2-MINI-1 datasheet v1.6 '
              'Fig. 11; derived from the KiCad ESP32-C6-MINI-1 footprint (identical land pattern). Keep the antenna area '
              'free of copper on all layers.")'
            + text[end:])
    return text


def sht4x_central_keepout() -> str:
    """Stock SHT4x DFN-4 pads, with the no-copper area reduced to the centre (under the sensor die) so tracks
    can reach every pad. The stock keepout wraps the pads and leaves only a 0.2 mm entry, which the
    autorouter cannot use."""
    src = pathlib.Path(STOCK_FP) / "Sensor_Humidity.pretty" / "Sensirion_DFN-4_1.5x1.5mm_P0.8mm_SHT4x_NoCentralPad.kicad_mod"
    text = src.read_text(encoding="utf-8")
    name = "Sensirion_DFN-4_1.5x1.5mm_P0.8mm_SHT4x_CentralKeepout"
    text = text.replace('(footprint "Sensirion_DFN-4_1.5x1.5mm_P0.8mm_SHT4x_NoCentralPad"', f'(footprint "{name}"', 1)
    start = text.index("(pts", text.index("(keepout"))
    end = text.index(")\n", text.index("(xy 0.75 -0.75)", start)) + 1
    text = text[:start] + "(pts (xy -0.35 -0.75) (xy 0.35 -0.75) (xy 0.35 0.75) (xy -0.35 0.75))" + text[end:]
    return text


FOOTPRINTS = {
    "Sensirion_DFN-4_1.5x1.5mm_P0.8mm_SHT4x_CentralKeepout": sht4x_central_keepout,
    "Texas_DLH0010A_WSON-10-1EP_2.2x2mm_P0.4mm": dlh0010a,
    "PIR_Panasonic_EKMB_TO-5": ekmb,
    "PIR_NEWOPTO_PIR223D_TO-5": pir223d,
    "CardEdge_sensor_edge_v1_2x10_P0.8mm": card_fingers,
    "Samtec_HSEC8-110-01-S-D-EM2": hsec8_em2,
    "ESP32-H2-MINI-1": esp32_h2_mini1,
}


def write_library(out_dir: pathlib.Path) -> list[str]:
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, fn in FOOTPRINTS.items():
        (out_dir / f"{name}.kicad_mod").write_text(fn(), encoding="utf-8")
    return list(FOOTPRINTS)
