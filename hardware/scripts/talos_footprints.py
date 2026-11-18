"""Project-local KiCad footprints (hardware/kicad/libraries/talos-footprints.pretty).

Sources:
- ESP32-H2-MINI-1: Espressif datasheet v1.6 Fig. 11 (pads 0.4 x 0.8 at 0.8 mm, columns 11.8 mm
  apart, rows 9.8 mm, corner pads 0.7 at 11.9 x 9.9, 3x3 1.45 mm EP array over 5.4 mm). That geometry
  is identical to KiCad's ESP32-C6-MINI-1 footprint, which is reused with the H2 description.
- BQ25186 DLH0010A: TI SLUSF69 §12 land pattern example (10 x 0.5 x 0.2 mm at 0.4 mm, 2.1 mm row
  span, EP 0.9 x 1.5 mm), modified: rows at +/-0.825 and EP 0.75 x 1.5 mm for 0.2 mm pad-to-EP copper.
- EKMB: Panasonic PaPIRs WL reference spec rev 7.1: three 0.45 mm leads on a 5.08 mm PCD; GND and OUT
  at +/-45 deg from the bottom vertical, Vdd opposite. Mapping VERIFY against a sample.
- sensor_edge_v1 card fingers: 0.80 mm pitch, 2 x 10 (odd top, even bottom), for a 1.60 mm card.
- HSEC8-110-01-S-D-EM2: PLACEHOLDER. Samtec's edge-mount drawing was not available; pad sizes and
  setback are assumptions so placement and routing can proceed. Replace before fabrication. (The
  slot socket moved from MEC8-EM to HSEC8-EM2 for 30 µin gold; same 0.80 mm 2 x 10 straddle concept,
  so the placeholder geometry is unchanged. HD-002.)
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


def card_fingers() -> str:
    body = ""
    for i in range(10):
        x = -3.6 + 0.8 * i
        body += _smd(str(2 * i + 1), x, 0, 0.5, 3.0, "F", paste=False, shape="rect")
        body += _smd(str(2 * i + 2), x, 0, 0.5, 3.0, "B", paste=False, shape="rect")
    body += _rect(-4.2, -1.75, 4.2, 1.75, "F.CrtYd", 0.05)
    body += _rect(-4.2, -1.75, 4.2, 1.75, "B.CrtYd", 0.05)
    body += _text("Talos_Note", "Board edge at y=+1.5; hard gold or ENIG TBD; chamfer 30 deg", 3.5, "F.Fab", True)
    return _footprint("CardEdge_sensor_edge_v1_2x10_P0.8mm",
                      "sensor_edge_v1 gold fingers, 2x10 at 0.80 mm, odd=top even=bottom, 1.60 mm card. Fingers end at y=+1.5 (card edge)",
                      "smd", body, -3, 3)


def hsec8_placeholder() -> str:
    body = ""
    for i in range(10):
        x = -3.6 + 0.8 * i
        body += _smd(str(2 * i + 1), x, 0, 0.45, 2.0, "F")
        body += _smd(str(2 * i + 2), x, 0, 0.45, 2.0, "B")
    body += _rect(-6.5, 1.0, 6.5, 6.5, "F.Fab")  # socket body overhanging the host edge (assumed)
    body += _rect(-6.75, -1.25, 6.75, 6.75, "F.CrtYd", 0.05)
    body += _rect(-6.75, -1.25, 6.75, 1.25, "B.CrtYd", 0.05)
    body += _line(-6.5, 1.0, 6.5, 1.0, "F.SilkS")
    return _footprint("Samtec_HSEC8-110-01-S-D-EM2_PLACEHOLDER",
                      "PLACEHOLDER for Samtec HSEC8-110-01-S-D-EM2 edge-mount socket: pads straddle the host edge (host edge at y=+1.0). "
                      "Pad size and setback are ASSUMED; replace with the Samtec drawing before fabrication (HD-002).",
                      "smd", body, -2.5, 8)


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
    "CardEdge_sensor_edge_v1_2x10_P0.8mm": card_fingers,
    "Samtec_HSEC8-110-01-S-D-EM2_PLACEHOLDER": hsec8_placeholder,
    "ESP32-H2-MINI-1": esp32_h2_mini1,
}


def write_library(out_dir: pathlib.Path) -> list[str]:
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, fn in FOOTPRINTS.items():
        (out_dir / f"{name}.kicad_mod").write_text(fn(), encoding="utf-8")
    return list(FOOTPRINTS)
