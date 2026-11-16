"""Project-local KiCad symbols for parts missing from the KiCad 10 stock libraries.

Every pin number/name below is transcribed from the datasheet named in `source`
(see hardware/requirements/datasheet-register.md). Graphics are a plain box.
"""

from __future__ import annotations

from kicad_sexpr import q

# (number, name, electrical type, side) ; side in L, R, T, B
SYMBOLS = {
    "ESP32-H2-MINI-1": {
        "ref": "U",
        "source": "Espressif ESP32-H2-MINI-1 & MINI-1U datasheet v1.6, Table 3 (53 pads)",
        "footprint": "talos:ESP32-H2-MINI-1",
        "pins": [
            ("3", "3V3", "power_in", "L"),
            ("15", "VBAT", "power_in", "L"),
            ("8", "EN", "input", "L"),
            ("9", "IO0", "bidirectional", "L"),
            ("10", "IO1", "bidirectional", "L"),
            ("5", "IO2", "bidirectional", "L"),
            ("6", "IO3", "bidirectional", "L"),
            ("18", "IO4", "bidirectional", "L"),
            ("19", "IO5", "bidirectional", "L"),
            ("22", "IO8", "bidirectional", "L"),
            ("23", "IO9", "bidirectional", "L"),
            ("20", "IO10", "bidirectional", "L"),
            ("21", "IO11", "bidirectional", "L"),
            ("16", "IO12", "bidirectional", "L"),
            ("12", "IO13", "bidirectional", "L"),
            ("13", "IO14", "bidirectional", "L"),
            ("24", "IO22", "bidirectional", "R"),
            ("30", "RXD0/IO23", "bidirectional", "R"),
            ("31", "TXD0/IO24", "bidirectional", "R"),
            ("25", "IO25", "bidirectional", "R"),
            ("26", "IO26/USB_D-", "bidirectional", "R"),
            ("27", "IO27/USB_D+", "bidirectional", "R"),
        ]
        + [(n, "NC", "no_connect", "R") for n in ("4", "7", "17", "28", "29", "32", "33", "34", "35")]
        + [(n, "GND", "power_in", "B") for n in ("1", "2", "11", "14")]
        + [(str(n), "GND", "power_in", "B") for n in range(36, 54)],
    },
    "BQ25186": {
        "ref": "U",
        "source": "TI SLUSF69A Table 4-1 + package drawing DLH0010A (pad 11 = thermal pad)",
        "footprint": "talos:Texas_DLH0010A_WSON-10-1EP_2.2x2mm_P0.4mm",
        "pins": [
            ("10", "IN", "power_in", "L"),
            ("4", "~{CE}", "input", "L"),
            ("6", "TS/MR", "bidirectional", "L"),
            ("7", "SDA", "bidirectional", "L"),
            ("8", "SCL", "input", "L"),
            ("1", "SYS", "power_out", "R"),
            ("2", "BAT", "power_in", "R"),
            ("3", "~{PG}/GPO", "open_collector", "R"),
            ("9", "~{INT}", "open_collector", "R"),
            ("5", "GND", "power_in", "B"),
            ("11", "EP", "passive", "B"),  # thermal pad (package drawing DLH0010A); must be soldered
        ],
    },
    "TPS2121": {
        "ref": "U",
        "source": "TI SLVSEA3F §6 Pin Functions, TPS2121 RUX VQFN-HR-12",
        "footprint": "Package_DFN_QFN:Texas_VQFN-HR-12_2x2.5mm_P0.5mm",
        "pins": [
            ("7", "IN1", "power_in", "L"),
            ("2", "IN2", "power_in", "L"),
            ("6", "PR1", "input", "L"),
            ("3", "CP2", "input", "L"),
            ("5", "OV1", "input", "L"),
            ("4", "OV2", "input", "L"),
            ("1", "OUT", "power_out", "R"),
            ("8", "OUT", "passive", "R"),  # second OUT pad; passive avoids a false power-out conflict
            ("9", "ST", "output", "R"),
            ("10", "ILIM", "passive", "R"),
            ("11", "SS", "passive", "R"),
            ("12", "GND", "power_in", "B"),
        ],
    },
    "LIS2DW12": {
        "ref": "U",
        "source": "ST DS11811 Rev 8 Table 1 (LGA-12)",
        "footprint": "Package_LGA:LGA-12_2x2mm_P0.5mm",
        "pins": [
            ("9", "VDD", "power_in", "L"),
            ("10", "VDD_IO", "power_in", "L"),
            ("1", "SCL/SPC", "input", "L"),
            ("4", "SDA/SDI/SDO", "bidirectional", "L"),
            ("2", "CS", "input", "L"),
            ("3", "SDO/SA0", "bidirectional", "L"),
            ("12", "INT1", "output", "R"),
            ("11", "INT2", "output", "R"),
            ("5", "NC", "passive", "R"),
            ("7", "RES", "passive", "R"),
            ("6", "GND", "power_in", "B"),
            ("8", "GND", "power_in", "B"),
        ],
    },
    "DRV5032xx-DBZ": {
        "ref": "U",
        "source": "TI SLVSDC7H Table 5-1 (SOT-23 DBZ: 1 VCC, 2 OUT, 3 GND; FA/FB/FC/AJ/ZE)",
        "footprint": "Package_TO_SOT_SMD:SOT-23",
        "pins": [
            ("1", "VCC", "power_in", "L"),
            ("2", "OUT", "output", "R"),
            ("3", "GND", "power_in", "B"),
        ],
    },
    "EKMB": {
        "ref": "U",
        "source": "Panasonic PaPIRs WL EKMB110111x reference spec rev 7.1 (bottom view: Vdd, OUT, GND; "
        "numbering is Talos convention, footprint must follow the drawing)",
        "footprint": "talos:PIR_Panasonic_EKMB_TO-5",
        "pins": [
            ("1", "Vdd", "power_in", "L"),
            ("2", "OUT", "output", "R"),
            ("3", "GND", "power_in", "B"),
        ],
    },
}

GRID = 2.54


def _pin_xy(side: str, idx: int, count: int, half_w: float, half_h: float):
    """Pin connection point and angle for a box symbol (symbol coords, y up)."""
    length = GRID
    if side in "LR":
        y = (count - 1) / 2 * GRID - idx * GRID
        x = -(half_w + length) if side == "L" else half_w + length
        return x, y, 0 if side == "L" else 180
    x = -(count - 1) / 2 * GRID + idx * GRID
    y = -(half_h + length) if side == "B" else half_h + length
    return x, y, 90 if side == "B" else 270


def build_symbol(name: str, spec: dict) -> list:
    groups = {s: [p for p in spec["pins"] if p[3] == s] for s in "LRTB"}
    rows = max(len(groups["L"]), len(groups["R"]), 1)
    cols = max(len(groups["T"]), len(groups["B"]), 1)
    half_h = (rows / 2 + 0.5) * GRID
    half_w = max(cols / 2 * GRID + GRID, 5 * GRID)
    half_w = round(half_w / GRID) * GRID

    def prop(key, value, y, hide=False):
        eff = ["effects", ["font", ["size", "1.27", "1.27"]]]
        if hide:
            eff.append(["hide", "yes"])
        return ["property", q(key), q(value), ["at", "0", f"{y:g}", "0"], eff]

    node = [
        "symbol",
        q(name),
        ["exclude_from_sim", "no"],
        ["in_bom", "yes"],
        ["on_board", "yes"],
        prop("Reference", spec["ref"], half_h + 1.27),
        prop("Value", name, -half_h - GRID * 2),
        prop("Footprint", spec["footprint"], -half_h - GRID * 3, True),
        prop("Datasheet", "", -half_h - GRID * 4, True),
        prop("Description", spec["source"], -half_h - GRID * 5, True),
    ]
    body = [
        "symbol",
        q(f"{name}_0_1"),
        [
            "rectangle",
            ["start", f"{-half_w:g}", f"{half_h:g}"],
            ["end", f"{half_w:g}", f"{-half_h:g}"],
            ["stroke", ["width", "0.254"], ["type", "default"]],
            ["fill", ["type", "background"]],
        ],
    ]
    pins = ["symbol", q(f"{name}_1_1")]
    for side, plist in groups.items():
        for i, (num, pname, etype, _) in enumerate(plist):
            x, y, ang = _pin_xy(side, i, len(plist), half_w, half_h)
            pins.append(
                [
                    "pin",
                    etype,
                    "line",
                    ["at", f"{x:g}", f"{y:g}", f"{ang:g}"],
                    ["length", f"{GRID:g}"],
                    ["name", q(pname), ["effects", ["font", ["size", "1.016", "1.016"]]]],
                    ["number", q(num), ["effects", ["font", ["size", "1.016", "1.016"]]]],
                ]
            )
    node += [body, pins, ["embedded_fonts", "no"]]
    return node


def library_node() -> list:
    lib = ["kicad_symbol_lib", ["version", "20241209"], ["generator", q("talos_gen")], ["generator_version", q("1.0")]]
    for name, spec in SYMBOLS.items():
        lib.append(build_symbol(name, spec))
    return lib
