"""Functional blocks of TL-SN-001, derived from the intended netlist (generated, like the projects).

    python3 hardware/scripts/layout_groups.py   # writes hardware/kicad/TL-SN-001/TL-SN-001.groups.json

The blocks are what a layout keeps together: a slot's switches, pull-ups, series resistors and TVS; the
3.3 V converter with its inductor and caps; the MCU with its strapping. The file is in KiCadRoutingTools'
groups format (github.com/drandyhaas/KiCadRoutingTools, MIT), which its placer reads; layout_search.py and
gen_pcbs.py read the same file, so both placers work from one set of blocks. A part goes to the first
block whose net rule it matches, in the order below; test pads follow their net.
"""

from __future__ import annotations

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from boards import kicad_dir  # noqa: E402

BOARD = "TL-SN-001"  # archived; kept so its frozen layout groups can be rebuilt
PLANE = {"GND", "V3V3_CORE"}

# (name, role, anchor, near, net patterns, why); order is priority
BLOCKS = [(f"slot{n}", "interface", f"J{4 + n}", ["bus", "regulator"],
           [rf"^SLOT{n}_", rf"^V3V3_AON_{n}$", rf"^V3V3_SW_{n}$", rf"^SW_ILIM{n}$", rf"^SLOT_AON_(EN|CT){n}$",
            rf"^SLOT_SW_EN{n}$", rf"^SLOT_WAKE{n}$"],
           f"slot {n}: HSEC8 socket, AON switch with CT cap and EN pull-down, SW current-limit switch with ILIM, "
           f"bleed and input cap, branch pull-ups, UART series and pull-ups, TVS")
          for n in range(4)] + [
    # M-06: every lamp on one edge strip, so one light pipe can serve them later
    ("leds", "interface", "D1", ["mcu", "bus"], [r"^STATUS_LED", r"^LED_K$", r"_LED_K$", r"^SLOT_LED"],
     "indicators: STAT (D1), PWR (D8, USB present) and the slot lamps S0-S3 with their resistors"),
    ("tamper", "interface", "J9", ["bus"], [r"^TAMPER_N$"], "enclosure tamper loop connector, pull-up, RC"),
    ("usb_in", "interface", "J1", ["pmux", "mcu"], [r"^USB_", r"^MCU_USB_"],
     "USB-C receptacle, CC pull-downs, USBLC6 ESD, USB data 0R links and DNP shunt caps"),
    ("pv_in", "interface", "J2", ["pmux"], [r"^PV_RAW$"], "PV input: connector, reverse diode, TVS"),
    ("pmux", "power", "U3", ["usb_in", "pv_in", "charger"], [r"^PMUX_", r"^PV_IN$", r"^CHG_IN$"],
     "TPS2121 power mux with priority divider, ILIM, soft-start, status pull-up and input caps"),
    ("charger", "power", "U4", ["pmux", "regulator"],
     [r"^CHG_", r"^BAT_", r"^VBAT$", r"^VBAT_PACK$", r"^BATFET_G$", r"^SYS_PATH$"],
     "BQ25186 linear charger, pack connector, reverse-pack P-FET and its gate resistor, NTC"),
    ("regulator", "power", "U5", ["charger", "mcu"], [r"^REG_", r"^SYS_REG$", r"^V3V3_REG$"],
     "TPS63900 buck-boost with inductor, CFG straps, input/output caps and the meter splits"),
    ("mcu", "core", "U1", ["bus", "usb_in"],
     [r"^CHIP_EN$", r"^BOOT_N$", r"^STATUS_LED", r"^LED_K$", r"^U0(RX|TX)D$", r"^VBAT_SENSE$"],
     "ESP32-H2 module with enable/boot strapping, VBAT divider, Tag-Connect pads and buttons"),
    ("bus", "core", "U14", ["mcu", "slot0", "slot1", "slot2", "slot3"],
     [r"^I2C_", r"^EXP_", r"^MUX_RESET_N$", r"^UART", r"^SPI_", r"^PRESENT_PROBE$", r"^SW_FAULT_N$"],
     "I2C mux, three expanders, interrupt inverter, UART mux, root and SPI pulls and their decaps"),
]


# Parts a net rule cannot place: ICs that touch several blocks' nets (the mux sees every slot branch),
# and parts whose only nets are planes (core-rail decaps, the input cap of each SW switch, test pads).
PINNED = {"U1": "mcu", "C1": "mcu", "C2": "mcu", "U2": "usb_in", "U3": "pmux", "U4": "charger",
          "U5": "regulator", "U14": "bus", "U15": "bus", "U16": "bus", "U17": "bus",
          "C21": "bus", "C22": "bus", "C23": "bus", "C24": "bus", "U18": "bus", "C37": "bus", "C36": "bus",
          "C35": "charger",  # BQ25186 IN decoupling (HD-037): it shares CHG_IN with the mux, belongs at U4
          "TP1": "regulator", "TP2": "mcu", "TP25": "bus", "TP26": "charger", "TP27": "pmux",
          **{f"C{31 + n}": f"slot{n}" for n in range(4)}}


def build() -> dict:
    d = json.load(open(kicad_dir(BOARD) / "intended-netlist.json"))
    parts = d["parts"] if isinstance(d["parts"], dict) else {p["ref"]: p for p in d["parts"]}
    members: dict[str, list[str]] = {b[0]: [] for b in BLOCKS}
    loose = []
    for ref in sorted(parts, key=lambda r: (re.sub(r"\d", "", r), int(re.sub(r"\D", "", r) or 0))):
        if ref in PINNED:
            members[PINNED[ref]].append(ref)
            continue
        nets = [n for n in parts[ref]["pins"].values() if n and n not in PLANE]
        for name, _role, anchor, _near, pats, _why in BLOCKS:
            if ref == anchor or any(re.search(p, n) for p in pats for n in nets):
                members[name].append(ref)
                break
        else:
            loose.append(ref)
    groups = []
    for name, role, anchor, near, _pats, why in BLOCKS:
        groups.append({"name": name, "role": role, "anchor": anchor, "near": near,
                       "refs": members[name], "why": why})
    return {"schema": 1, "board": f"{BOARD}.kicad_pcb", "groups": groups, "_loose": loose,
            "note": "Generated by hardware/scripts/layout_groups.py; edit the script, not this file."}


def main() -> int:
    g = build()
    loose = g.pop("_loose")
    sys.path.insert(0, str(ROOT / "hardware" / "scripts"))
    if loose:
        print(f"{len(loose)} part(s) match no block rule: {' '.join(loose)}")
    out = kicad_dir(BOARD) / f"{BOARD}.groups.json"
    out.write_text(json.dumps(g, indent=1) + "\n")
    for grp in g["groups"]:
        print(f"{grp['name']:10s} {len(grp['refs']):3d}  {' '.join(grp['refs'])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
