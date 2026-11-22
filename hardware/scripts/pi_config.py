"""Write the kisi power-integrity configuration for TL-SN-001 (generated, like the KiCad projects).

    python3 hardware/scripts/pi_config.py           # writes hardware/kicad/TL-SN-001/TL-SN-001.kisi.json
    python3 hardware/scripts/pi_config.py --case pv # the solar-input case instead (HD-041); run kisi, then
                                                    # regenerate the default (USB) file before committing
    node ~/.cache/talos-tools/kisi-bin/kisi.mjs power   hardware/kicad/TL-SN-001/TL-SN-001.kicad_pcb
    node ~/.cache/talos-tools/kisi-bin/kisi.mjs thermal hardware/kicad/TL-SN-001/TL-SN-001.kicad_pcb

kisi (github.com/riehlies/kisi, GPL-3.0, run as an external tool; nothing of it is vendored) reads
<board>.kisi.json next to the board. This file describes the USB-powered worst case: USB in through the
TPS2121 and BQ25186, the charger charging at 300 mA, the radio transmitting, and the slots drawing
what the firmware's 800 mW admission budget allows.
kisi has one power tree per board, so battery-only operation is not modelled here (power_budget.py covers it).
Every value carries a source; kind "estimate"/"assumption" marks what the bench must replace.
"""

from __future__ import annotations

import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = "TL-SN-001"


def src(kind: str, note: str) -> dict:
    return {"kind": kind, "note": note}


DS = "datasheet"
FIELDS = [
    # Power tree, USB case (kisi PI_REG syntax: INPUT > OUTPUT kind values)
    ("PI_REG", "J1", "/USB_VBUS@5V input", src("standard", "USB 2.0 VBUS 5 V nominal")),
    ("PI_REG", "R68", "/USB_VBUS > /PMUX_IN1 pass r=50m", src("assumption", "0R jumper, 50 mOhm max class")),
    ("PI_REG", "U3", "/PMUX_IN1 > /CHG_IN pass r=56m", src(DS, "TPS2121 RON 56 mOhm typ (SLVSEA3F device comparison)")),
    ("PI_REG", "U4", "/CHG_IN > /SYS_PATH pass id=U4-SYS; /CHG_IN > /VBAT pass id=U4-CHG",
     src("assumption", "BQ25186 linear power path; input FET resistance not modelled")),
    ("PI_REG", "Q1", "/VBAT > /VBAT_PACK pass r=50m",
     src(DS, "AO3401A RDS(ON) < 50 mOhm at VGS = -4.5 V (AOS datasheet); reverse-pack FET, gate pulled to GND")),
    ("PI_REG", "R70", "/SYS_PATH > /SYS_REG pass r=50m", src("assumption", "0R meter split, 50 mOhm max class")),
    ("PI_REG", "U5", "/SYS_REG > /V3V3_REG buck-boost 90% 400mA",
     src(DS, "TPS63900 IO 0.4 A (SLVSET3D 6.3); 90% efficiency is the light-load claim, EST at full load")),
    ("PI_REG", "R71", "/V3V3_REG > /V3V3_CORE pass r=50m", src("assumption", "0R meter split, 50 mOhm max class")),
] + [
    ("PI_REG", f"U{6 + i}", f"/V3V3_CORE > /V3V3_AON_{i} pass r=90m",
     src(DS, "TPS22917 RON 90 mOhm typ at 3.6 V (SLVSDW8B 6.5)")) for i in range(4)
] + [
    ("PI_REG", f"U{10 + i}", f"/V3V3_CORE > /V3V3_SW_{i} pass r=85m",
     src(DS, "TPS2553 DBV rDS(on) 85 mOhm typ (SLVS841G 6.5)")) for i in range(4)
] + [
    # Loads
    ("PI_LOAD", "U1", "/V3V3_CORE=153mA", src("estimate", "ESP32-H2 30 mA active + 123 mA TX at 18 dBm (power-budget.md)")),
    ("PI_LOAD", "J3", "/VBAT_PACK=300mA", src("design", "ICHG 300 mA on USB (power-budget.md charger policy)")),
    ("PI_THERMAL", "U4", "p=0.42W",
     src("simulation", "linear charger (VIN - VBAT) x ICHG at 5 V, 300 mA (power-budget.md section 3)")),
    # Load power leaves the board (into the pack, onto the cards): no heat in the connectors themselves.
    ("PI_THERMAL", "J3", "p=0W", src("design", "charge energy is stored in the pack, not dissipated in J3")),
] + [
    ("PI_THERMAL", f"J{4 + i}", "p=0W", src("design", "card load dissipates on the card")) for i in range(4)
] + [
    ("PI_LOAD", f"J{4 + i}", f"/V3V3_AON_{i}=5mA, /V3V3_SW_{i}=55mA",
     src("design", "firmware admission caps module load at usb_sustained_mw = 800 mW: 4 x 55 mA x 3.3 V "
                   "= 726 mW (board_tl_sn_001.h, finding S-2); AON 5 mA EST")) for i in range(4)
]


# Solar input (HD-041): PV in through D2 and the TPS2121's IN2, charger dissipation at 6 V. Replaces the USB
# entries for the source, the mux path and U4's heat; the loads are the same.
PV_FIELDS = [
    ("PI_REG", "J2", "/PV_RAW@6V input", src("assumption", "HD-019: '6 V nominal' panel, loaded")),
    ("PI_REG", "D2", "/PV_RAW > /PV_IN pass r=100m", src("estimate", "SS34 at ~0.3 A: ~0.35 V, as 100 mOhm EST")),
    ("PI_REG", "U3", "/PV_IN > /CHG_IN pass r=56m", src(DS, "TPS2121 RON 56 mOhm typ (SLVSEA3F)")),
    ("PI_THERMAL", "U4", "p=0.72W",
     src("simulation", "linear charger (6 V - 3.6 V) x 300 mA = 0.72 W before the firmware PV ICHG cap "
                       "(power-tree.md); the worst case HD-041 asks about")),
]


# TL-SN-002 (ADR 0019): 2 x AA through the reverse-cell P-FET and the TPS2116, or USB; one TPS63900.
# Default case "battery": cells at the 2.2 V end-of-life threshold (highest currents in the power path) with
# the radio transmitting. Case "usb": 5 V in, every lamp lit (lamps run only on USB or in service mode).
SN2_COMMON = [
    ("PI_REG", "R70", "/VSYS > /SYS_REG pass r=50m", src("assumption", "0R meter split, 50 mOhm max class")),
    ("PI_REG", "U5", "/SYS_REG > /V3V3_REG buck-boost 85% 400mA",
     src(DS, "TPS63900 IO 0.4 A (SLVSET3D 6.3); 85% EST at 2.2 V in, 150 mA out")),
    ("PI_REG", "R71", "/V3V3_REG > /V3V3 pass r=50m", src("assumption", "0R meter split, 50 mOhm max class")),
    ("PI_THERMAL", "U5", "p=0.09W", src("simulation", "TPS63900 loss = 0.5 W out x (1/0.85 - 1)")),
    ("PI_THERMAL", "BT1", "p=0W", src("design", "cell self-heating ignored at these currents")),
    # listed so kisi reports the board temperature at the sensor (HD-047); its own heat is ~0.5 mW
    ("PI_THERMAL", "U9", "p=0.0005W", src(DS, "SHT40 0.5 mA x 3.3 V while measuring, low duty: upper bound")),
]
SN2_FIELDS = {
    "battery": [
        ("PI_REG", "BT1", "/BATT_RAW@2.2V input", src("design", "2 x AA at the firmware low-battery threshold")),
        ("PI_REG", "Q1", "/BATT_RAW > /VBAT pass r=60m", src(DS, "AO3401A RDS(ON) at VGS -2.5 V (AOS), EST")),
        ("PI_REG", "U3", "/VBAT > /VSYS pass r=61m", src(DS, "TPS2116 RON 61 mOhm max at 1.8 V, 85 C (SLVSFG1A 6.5)")),
        ("PI_LOAD", "U1", "/V3V3=153mA", src("estimate", "ESP32-H2 30 mA active + 123 mA TX at 18 dBm")),
    ] + SN2_COMMON,
    "usb": [
        ("PI_REG", "J1", "/USB_VBUS@5V input", src("standard", "USB 2.0 VBUS 5 V nominal")),
        ("PI_REG", "U3", "/USB_VBUS > /VSYS pass r=55m", src(DS, "TPS2116 RON 55 mOhm max at 5 V, 85 C (SLVSFG1A 6.5)")),
        ("PI_LOAD", "U1", "/V3V3=153mA", src("estimate", "ESP32-H2 30 mA active + 123 mA TX at 18 dBm")),
        ("PI_LOAD", "U14", "/V3V3=7mA", src("design", "8 expander lamps at ~0.6 mA plus STAT ~1 mA (spice B6)")),
        ("PI_LOAD", "D8", "/USB_VBUS=1mA", src("design", "PWR lamp from VBUS, 3.3k")),
    ] + SN2_COMMON,
}


def fields(case: str) -> list:
    if case == "usb":
        return FIELDS
    drop = {("PI_REG", "J1"), ("PI_REG", "R68"), ("PI_REG", "U3"), ("PI_THERMAL", "U4")}
    return [f for f in FIELDS if (f[0], f[1]) not in drop] + PV_FIELDS


def build(case: str = "usb", board: str = BOARD) -> dict:
    rows = SN2_FIELDS[case] if board == "TL-SN-002" else fields(case)
    return {
        "kisi_config": 1,
        "board": f"{board}.kicad_pcb",
        "notes": ["Generated by hardware/scripts/pi_config.py; edit the script, not this file.", f"case: {case}"],
        "fields": [{"name": n, "ref": r, "value": val, "source": s} for n, r, val, s in rows],
    }


def main() -> int:
    out = ROOT / "hardware" / "kicad" / BOARD / f"{BOARD}.kisi.json"
    import sys

    board = sys.argv[sys.argv.index("--board") + 1] if "--board" in sys.argv else None
    case = sys.argv[sys.argv.index("--case") + 1] if "--case" in sys.argv else None
    # Default (CI): both boards in their committed cases. --board/--case write one board for a kisi run.
    todo = [(board, case or ("battery" if board == "TL-SN-002" else "usb"))] if board else \
        [(BOARD, "usb"), ("TL-SN-002", "battery")]
    for b, c in todo:
        if c not in (("battery", "usb") if b == "TL-SN-002" else ("usb", "pv")):
            raise SystemExit(f"{b}: unknown case {c}")
        path = ROOT / "hardware" / "kicad" / b / f"{b}.kisi.json"
        cfg = build(c, b)
        path.write_text(json.dumps(cfg, indent=2) + "\n")
        print(f"wrote {path.relative_to(ROOT)} ({len(cfg['fields'])} fields, {c} case)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
