#!/usr/bin/env python3
"""Generate the Talos H3 KiCad schematics from one declarative netlist.

    python3 hardware/scripts/gen_schematics.py

Writes hardware/kicad/<board>/<board>.kicad_{sch,pro}, sym/fp-lib-tables, the
project symbol library, and hardware/kicad/<board>/intended-netlist.json (the
connectivity check_kicad.sh compares KiCad's exported netlist against).

Each pin is joined to its net by a short wire stub plus a local label, so the
drawing is a "labelled pin" schematic: correct and checkable, not pretty. Do not
hand-edit generated files; change this script.
"""

from __future__ import annotations

import json
import os
import pathlib
import sys
import uuid
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from kicad_sexpr import SymbolLibraries, dump, pin_end, q, stub_dir  # noqa: E402
from talos_footprints import write_library  # noqa: E402
from talos_symbols import SYMBOLS, library_node  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
KICAD_DIR = ROOT / "hardware" / "kicad"
LIB_DIR = KICAD_DIR / "libraries"
TALOS_LIB = LIB_DIR / "talos-symbols.kicad_sym"
STOCK = os.environ.get(
    "KICAD_SYMBOL_DIR",
    os.path.expanduser("~/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols"),
)
NS = uuid.UUID("6f6b0a52-6a35-4d7e-9f0e-7a1c9e3d2b10")
G = 2.54

FP_R = "Resistor_SMD:R_0402_1005Metric"
FP_R_POWER = "Resistor_SMD:R_0805_2012Metric"  # 0R links in the power path (>0.5 A)
FP_HDR2 = "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical"
FP_C = "Capacitor_SMD:C_0402_1005Metric"
FP_C_BULK = "Capacitor_SMD:C_0805_2012Metric"
FP_TP = "TestPoint:TestPoint_Pad_D1.0mm"
FP_LED = "LED_SMD:LED_0603_1608Metric"
FP_TAG = "Connector:Tag-Connect_TC2030-IDC-NL_2x03_P1.27mm_Vertical"
FP_HOOK = "TestPoint:TestPoint_Keystone_5000-5004_Miniature"
# Footprints that are copper/holes only (nothing to buy): kept out of the BOM like the test pads.
FP_SOLDER_JUMPER = "Jumper:SolderJumper-2_P1.3mm_Bridged_RoundedPad1.0x1.5mm"
# Copper-only footprints: no part to buy or place, so no BOM line (the stock footprints say so too).
BOARD_FEATURES = (FP_TP, FP_TAG, FP_SOLDER_JUMPER)


@dataclass
class Part:
    ref: str
    lib_id: str
    value: str
    pins: dict[str, str | None]  # pin number -> net (None = intentionally unconnected)
    footprint: str | None = None  # None = use symbol default
    note: str = ""
    dnp: bool = False  # footprint on the board, not fitted (bench/test option)


@dataclass
class Board:
    name: str
    title: str
    parts: list[Part] = field(default_factory=list)
    external_power: set[str] = field(default_factory=set)  # nets fed from connectors/batteries

    def add(self, *parts: Part) -> None:
        self.parts.extend(parts)


def R(ref, value, a, b, note="", power=False):
    return Part(ref, "Device:R", value, {"1": a, "2": b}, FP_R_POWER if power else FP_R, note)


def C(ref, value, a, b="GND", bulk=False, dnp=False, note=""):
    return Part(ref, "Device:C", value, {"1": a, "2": b}, FP_C_BULK if bulk else FP_C, note, dnp)


def SPLIT(r_ref, jp_ref, a, b, what):
    """0R link with a DNP 2-pin header across it: lift the 0R, fit the header, meter the block (bench §8)."""
    return [R(r_ref, "0R", a, b, f"{what}: lift to isolate/meter (header {jp_ref} across it)", power=True),
            Part(jp_ref, "Connector_Generic:Conn_01x02", "DNP meter", {"1": a, "2": b}, FP_HDR2,
                 f"{what}: fit a header and a PPK2 after lifting {r_ref}", dnp=True)]


def TP(ref, net):
    return Part(ref, "Connector:TestPoint", net, {"1": net}, FP_TP)


def LED(d_ref, r_ref, label, anode, drive, ohms, colour="LED_GREEN", note=""):
    """Indicator: LED from `anode`, cathode through `ohms` to `drive` (a sink, or GND for a lamp that is lit
    whenever `anode` is up). High-efficiency 0603 at 0.5-1 mA (board-practices-review.md, light rules)."""
    k = f"{label}_LED_K"
    return [Part(d_ref, "Device:LED", colour, {"2": anode, "1": k}, FP_LED, note or f"{label} indicator"),
            R(r_ref, ohms, k, drive, f"{label} LED current")]


# ---- sensor_edge_v1 -------------------------------------------------------------------
# contact -> role (interfaces/sensor_edge_v1.md). Same table for main board and cards. PRESENT_N is on
# contact 19 (HD-025): a card inserted upside down maps contact c to 21 - c, so its grounded PRESENT_N
# lands on host GND (2) instead of the slot's AON rail (3, as it did from contact 18).
EDGE = {
    1: "GND", 2: "GND", 3: "AON", 4: "SW", 5: "V5_RSVD", 6: "GND", 7: "SDA", 8: "SCL",
    9: "WAKE", 10: "ALERT_N", 11: "SCK", 12: "MOSI", 13: "MISO", 14: "CS_N", 15: "UART_TX",
    16: "UART_RX", 17: "RESET_N", 18: "ANALOG_RSVD", 19: "PRESENT_N", 20: "GND",
}


def main_slot_net(n: int, role: str) -> str | None:
    return {
        "GND": "GND", "AON": f"V3V3_AON_{n}", "SW": f"V3V3_SW_{n}", "V5_RSVD": None,
        "SDA": f"SLOT{n}_SDA", "SCL": f"SLOT{n}_SCL", "WAKE": f"SLOT_WAKE{n}",
        "ALERT_N": f"SLOT{n}_ALERT_N", "SCK": "SPI_SCK", "MOSI": "SPI_MOSI", "MISO": "SPI_MISO",
        "CS_N": f"SLOT{n}_CS_N", "UART_TX": f"SLOT{n}_UART_TX", "UART_RX": f"SLOT{n}_UART_RX",
        "RESET_N": f"SLOT{n}_RESET_N", "PRESENT_N": f"SLOT{n}_PRESENT_N", "ANALOG_RSVD": None,
    }[role]


def card_edge(used: set[str]) -> Part:
    """Card gold fingers. Unused roles are left unconnected on the card."""
    pins = {}
    for c, role in EDGE.items():
        if role == "GND":
            pins[str(c)] = "GND"
        elif role == "PRESENT_N":
            pins[str(c)] = "GND"  # every card ties PRESENT_N low (S-1)
        elif role in used:
            pins[str(c)] = {"AON": "V3V3_AON", "SW": "V3V3_SW"}.get(role, role)
        else:
            pins[str(c)] = None
    return Part("J1", "Connector_Generic:Conn_02x10_Odd_Even", "sensor_edge_v1 card fingers", pins,
                "talos:CardEdge_sensor_edge_v1_2x10_P0.8mm", "gold fingers, 1.60 mm card, 0.80 mm pitch")


def id_eeprom(ref="U1", cap="C1") -> list[Part]:
    return [
        Part(ref, "Memory_EEPROM:24AA025E-OT", "24AA025E48T-I/OT",
             {"1": "SCL", "2": "GND", "3": "SDA", "4": "GND", "5": "GND", "6": "V3V3_AON"},
             note="ID EEPROM 0x50 with factory EUI-48 (S-1)"),
        C(cap, "100n", "V3V3_AON"),
    ]


# ---- TL-SN-001 ------------------------------------------------------------------------
def sensor_node() -> Board:
    b = Board("TL-SN-001", "TL-SN-001 Sensor Node Rev A (generated, provisional)",
              # Nets reached only through a 0R/FET (passive) also need a flag for ERC.
              external_power={"GND", "USB_VBUS", "PV_IN", "VBAT_PACK", "VBAT", "PMUX_IN1", "SYS_REG",
                              "V3V3_CORE"})
    core = "V3V3_CORE"

    # Sheet A: MCU, boot, reset, USB-JTAG, status
    mcu = {
        "3": core, "15": core, "8": "CHIP_EN", "9": "SPI_MISO", "10": "VBAT_SENSE",
        "5": "I2C_SDA", "6": "I2C_SCL", "18": "SPI_SCK", "19": "SPI_MOSI", "22": "STATUS_LED_N",
        "23": "BOOT_N", "20": "SLOT_WAKE0", "21": "SLOT_WAKE1", "16": "SLOT_WAKE2",
        "12": "SLOT_WAKE3", "13": "EXP_WAKE", "24": "UART1_TX", "25": "UART1_RX",
        "30": "U0RXD", "31": "U0TXD", "26": "MCU_USB_DN", "27": "MCU_USB_DP",
    }
    for n in ("4", "7", "17", "28", "29", "32", "33", "34", "35"):
        mcu[n] = None
    for n in ["1", "2", "11", "14"] + [str(i) for i in range(36, 54)]:
        mcu[n] = "GND"
    b.add(
        Part("U1", "talos:ESP32-H2-MINI-1", "ESP32-H2-MINI-1-H4S", mcu, None,
             "land pattern = datasheet Fig. 11; antenna keepout at board edge"),
        C("C1", "10u", core, bulk=True), C("C2", "100n", core),
        R("R1", "10k", core, "CHIP_EN", "EN RC per datasheet §9"), C("C3", "1u", "CHIP_EN"),
        Part("SW1", "Switch:SW_Push", "RESET", {"1": "CHIP_EN", "2": "GND"}, "Button_Switch_SMD:SW_SPST_PTS810"),
        R("R2", "10k", core, "BOOT_N", "GPIO9 strap pull-up (SPI boot)"),
        Part("SW2", "Switch:SW_Push", "BOOT", {"1": "BOOT_N", "2": "GND"}, "Button_Switch_SMD:SW_SPST_PTS810"),
        R("R3", "10k", core, "STATUS_LED_N", "GPIO8 strap held high"),
        Part("D1", "Device:LED", "STATUS", {"2": core, "1": "LED_K"}, FP_LED, "STAT: firmware blink codes (L-01)"),
        R("R4", "1k", "LED_K", "STATUS_LED_N"),
        R("R5", "4.7k", core, "I2C_SDA"), R("R6", "4.7k", core, "I2C_SCL"),
        R("R7", "4.7M", "VBAT", "VBAT_SENSE"), R("R8", "4.7M", "VBAT_SENSE", "GND"),
        C("C4", "100n", "VBAT_SENSE"),
        R("R9", "47k", core, "EXP_INT_N"),
        # Stuck-bus recovery (owner decision 1): GPIO23 drives the mux reset. Lift R65 to use
        # U0RXD as a console instead (R33 then holds the mux out of reset).
        R("R65", "0R", "U0RXD", "MUX_RESET_N", "GPIO23 -> TCA9546A RESET_N; lift for UART0 RX on J10"),
        # USB D+/D-: 0R links with DNP shunt-C pads on the module side (Espressif HW design guide)
        R("R66", "0R", "MCU_USB_DP", "USB_DP"), R("R67", "0R", "MCU_USB_DN", "USB_DN"),
        C("C25", "DNP", "MCU_USB_DP", dnp=True, note="USB D+ shunt C option (Espressif)"),
        C("C26", "DNP", "MCU_USB_DN", dnp=True, note="USB D- shunt C option (Espressif)"),
        # D-01: Tag-Connect TC2030-NL pads (no part) replace the DNP UART0 header: pogo-pin flashing and
        # recovery when USB is dead or the USB-JTAG pins were reconfigured. Lift R65 before driving RX.
        Part("J10", "Connector_Generic:Conn_01x06", "TC2030 PROG",
             {"1": core, "2": "CHIP_EN", "3": "GND", "4": "U0TXD", "5": "BOOT_N", "6": "U0RXD"}, FP_TAG,
             "Tag-Connect TC2030-NL: 1 3V3, 2 EN, 3 GND, 4 U0TXD, 5 BOOT (GPIO9), 6 U0RXD"),
        # L-02: USB-present lamp, lit straight from VBUS: free on battery, no GPIO
        *LED("D8", "R94", "PWR", "USB_VBUS", "GND", "3.3k", note="PWR: USB present (hardware, ~0.9 mA from VBUS)"),
    )

    # Sheet B: USB-C, PV, power mux, charger, battery
    b.add(
        Part("J1", "Connector:USB_C_Receptacle_USB2.0_16P", "USB-C sink",
             {"A1": "GND", "B1": "GND", "A12": "GND", "B12": "GND", "SH": "GND",
              "A4": "USB_VBUS", "A9": "USB_VBUS", "B4": "USB_VBUS", "B9": "USB_VBUS",
              "A5": "USB_CC1", "B5": "USB_CC2", "A6": "USB_DP", "B6": "USB_DP",
              "A7": "USB_DN", "B7": "USB_DN", "A8": None, "B8": None},
             "Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal"),
        R("R10", "5.1k", "USB_CC1", "GND", "UFP Rd"), R("R11", "5.1k", "USB_CC2", "GND", "UFP Rd"),
        Part("U2", "Power_Protection:USBLC6-2SC6", "USBLC6-2SC6",
             {"1": "USB_DP", "6": "USB_DP", "3": "USB_DN", "4": "USB_DN", "5": "USB_VBUS", "2": "GND"}),
        C("C5", "4.7u", "USB_VBUS", bulk=True),
        Part("J2", "Connector_Generic:Conn_01x02", "PV in", {"1": "PV_RAW", "2": "GND"},
             "Connector_JST:JST_PH_S2B-PH-SM4-TB_1x02-1MP_P2.00mm_Horizontal",
             "placeholder connector; outdoor-rated keyed jack TBD"),
        Part("D2", "Diode:SS34", "SS34", {"2": "PV_RAW", "1": "PV_IN"}, None, "PV reverse-polarity block"),
        Part("D3", "Diode:SMAJ12A", "SMAJ12A", {"1": "PV_IN", "2": "GND"}, None,
             "TVS: pin 1 is the cathode (bar), so it sits on PV_IN and only conducts above breakdown"),
        C("C6", "10u/25V", "PV_IN", bulk=True),
        Part("U3", "talos:TPS2121", "TPS2121RUXR",
             {"7": "PMUX_IN1", "2": "PV_IN", "6": "PMUX_PR1", "3": "GND", "5": "GND", "4": "GND",
              "1": "CHG_IN", "8": "CHG_IN", "9": "PMUX_ST", "10": "PMUX_ILIM", "11": "PMUX_SS", "12": "GND"}),
        R("R12", "100k", "PMUX_ILIM", "GND", "ILIM = 65.2/R^0.861 = 1.24 A typ (SLVSEA3F eq. 2; 18-100k range)"),
        C("C7", "100n", "PMUX_SS"),  # ~780 V/s output slew (SLVSEA3F Table 9-1)
        R("R68", "0R", "USB_VBUS", "PMUX_IN1", "lift to isolate the USB input from the power mux", power=True),
        # 37 kOhm total so board leakage when hot can't hold PR1 up without USB (owner decision 4).
        # PR1 = 10/37 of VBUS: takes USB above 3.92 V typ (VREF 1.06 V, 1.01-1.10 V: 3.68-4.13 V
        # with 1% resistors; spice_sim.py B5, HD-007).
        R("R13", "27k", "PMUX_IN1", "PMUX_PR1", "priority divider (HD-007)"),
        R("R14", "10k", "PMUX_PR1", "GND", "priority divider (HD-007)"),
        R("R15", "10k", core, "PMUX_ST", "ST pull-up: datasheet range 6-20k"),
        C("C8", "4.7u/25V", "CHG_IN", bulk=True),
        # HD-037: local input cap on the BQ25186 IN pin (C8 sits at the power mux)
        C("C35", "1u/25V", "CHG_IN", note="BQ25186 IN local decoupling, at the pin (HD-037)"),
        Part("U4", "talos:BQ25186", "BQ25186DLHR",
             {"10": "CHG_IN", "1": "SYS_PATH", "2": "VBAT", "3": "CHG_PG_N", "4": "GND", "5": "GND",
              "6": "BAT_NTC", "7": "I2C_SDA", "8": "I2C_SCL", "9": "CHG_INT_N", "11": "GND"}, None,
             "DLH0010A land pattern from SLUSF69 §12"),
        # C10: 1 uF 0402 keeps only ~0.5 uF at 4.2 V; 2.2 uF 0805 (owner decision 5)
        C("C9", "10u", "SYS_PATH", bulk=True), C("C10", "2.2u/25V", "VBAT", bulk=True),
        R("R16", "100k", core, "CHG_PG_N"), R("R17", "10k", core, "CHG_INT_N"),
        # HD-029: fit for a pack without an NTC (a floating TS reads as a fault and stops charging)
        Part("R100", "Device:R", "10k", {"1": "BAT_NTC", "2": "GND"}, FP_R,
             "fit only for an NTC-less pack (HD-029)", dnp=True),
        Part("J3", "Connector_Generic:Conn_01x03", "1S pack (BAT+, NTC, GND)",
             {"1": "VBAT_PACK", "2": "BAT_NTC", "3": "GND"},
             "Connector_JST:JST_PH_S3B-PH-SM4-TB_1x03-1MP_P2.00mm_Horizontal",
             "keyed protected-pack connector TBD (HD-006); 4-pin with separate NTC return once the pack is chosen"),
        # Reverse-pack protection (owner decision 6): body diode conducts for a correct pack, then the
        # gate at GND turns the channel on (both directions). Reversed pack: diode blocks, Vgs ~ 0.
        # Caveat HD-024: with USB in, the charger can raise VBAT and turn Q1 on into a reversed pack.
        Part("Q1", "Transistor_FET:AO3401A", "AO3401A", {"1": "BATFET_G", "2": "VBAT", "3": "VBAT_PACK"},
             "Package_TO_SOT_SMD:SOT-23", "reverse-pack P-FET: D = pack, S = VBAT"),
        R("R69", "100k", "BATFET_G", "GND", "Q1 gate"),
    )

    # Sheet C: 3.3 V regulator and per-slot switches
    b.add(
        Part("U5", "Regulator_Switching:TPS63900", "TPS63900DSKR",
             {"10": "SYS_REG", "1": "SYS_REG", "2": "GND", "3": "REG_CFG1", "4": "REG_CFG2",
              "5": "REG_CFG3", "6": "V3V3_REG", "9": "REG_LX1", "7": "REG_LX2", "8": "GND", "11": "GND"},
             "Package_SON:WSON-10-1EP_2.5x2.5mm_P0.5mm_EP1.2x2mm"),
        R("R18", "36.5k 1%", "REG_CFG1", "GND", "SEL-high 3.3 V, unlimited ILIM (Table 7-2)"),
        R("R19", "0R", "REG_CFG2", "GND", "SEL-high 3.3 V, unlimited ILIM (Table 7-2)"),
        R("R20", "16.2k 1%", "REG_CFG3", "GND", "SEL-low 3.3 V (Table 7-3)"),
        Part("L1", "Device:L", "2.2u DFE252012F-2R2M", {"1": "REG_LX1", "2": "REG_LX2"},
             "Inductor_SMD:L_1008_2520Metric"),
        C("C11", "10u", "SYS_REG", bulk=True), C("C12", "22u", "V3V3_REG", bulk=True),
        # Bench splits: charger | regulator | loads, each block powered and metered alone (bench §8)
        *SPLIT("R70", "JP1", "SYS_PATH", "SYS_REG", "charger to regulator"),
        *SPLIT("R71", "JP2", "V3V3_REG", core, "regulator to 3.3 V loads"),
    )
    for n in range(4):
        b.add(
            Part(f"U{6 + n}", "Power_Management:TPS22917DBV", "TPS22917DBVR",
                 # HD-028: QOD to VOUT, so the rail discharges on-chip when switched off
                 {"1": core, "2": "GND", "3": f"SLOT_AON_EN{n}", "4": f"SLOT_AON_CT{n}", "5": f"V3V3_AON_{n}",
                  "6": f"V3V3_AON_{n}"},
                 note=f"slot {n} AON switch, default off; firmware powers seated slots one at a time"),
            # Owner decision 2: default off, so a shorted card can't hold the node in a boot loop
            R(f"R{21 + n}", "1M", f"SLOT_AON_EN{n}", "GND", "AON default off"),
            C(f"C{13 + n}", "1u", f"V3V3_AON_{n}"),
            # CT goes to VIN, not GND (SLVSDW8B 9.3.2). 1 nF: ~1.9 mV/us, inrush 26 mA instead of 0.5 A
            # into a 10 uF card, which could brown out the ESP32 (spice_sim.py B1, finding S-1).
            C(f"C{27 + n}", "1n", f"SLOT_AON_CT{n}", core, note="TPS22917 CT to VIN: slow AON ramp"),
            # Owner decision 3: current-limited switch with FAULT on SW (TPS22917 has no limit)
            Part(f"U{10 + n}", "talos:TPS2553", "TPS2553DBVR-1",
                 {"1": core, "2": "GND", "3": f"SLOT_SW_EN{n}", "4": "SW_FAULT_N", "5": f"SW_ILIM{n}",
                  "6": f"V3V3_SW_{n}"},
                 note=f"slot {n} switched rail, default off, latch-off current limit"),
            R(f"R{25 + n}", "1M", f"SLOT_SW_EN{n}", "GND"),
            R(f"R{29 + n}", "150k 1%", f"SW_ILIM{n}", "GND",
              "IOS 155-207 mA min-max (SLVS841G eqs 1/3) > 150 mA per-slot SW budget"),
            R(f"R{72 + n}", "100k", f"V3V3_SW_{n}", "GND", "SW bleed: TPS2553 has no output discharge"),
            C(f"C{17 + n}", "1u", f"V3V3_SW_{n}"),
            C(f"C{31 + n}", "100n", core),
        )
    b.add(R("R76", "100k", core, "SW_FAULT_N", "wired-OR FAULT_N pull-up"),
          # HD-027: SPI held low while the ESP32 can't hold GPIO0-5 (deep sleep), so an unpowered card is
          # not back-powered through its SCK/MOSI/MISO clamps. GPIO4 boots with its pull-up on (~2.3 V
          # against 100k) until firmware drives it.
          R("R101", "100k", "SPI_SCK", "GND", "HD-027"), R("R102", "100k", "SPI_MOSI", "GND", "HD-027"),
          R("R103", "100k", "SPI_MISO", "GND", "HD-027"))

    # Sheet D: I2C mux, expanders, UART mux
    mux = {"1": "GND", "2": "GND", "13": "GND", "3": "MUX_RESET_N", "14": "I2C_SCL", "15": "I2C_SDA",
           "16": core, "8": "GND"}
    for n, (sd, sc) in enumerate([("4", "5"), ("6", "7"), ("9", "10"), ("11", "12")]):
        mux[sd], mux[sc] = f"SLOT{n}_SDA", f"SLOT{n}_SCL"
    b.add(Part("U14", "Interface_Expansion:TCA9546APW", "TCA9546APWR", mux, note="root 0x70"),
          R("R33", "100k", core, "MUX_RESET_N"), C("C21", "100n", core))
    for n in range(4):
        b.add(R(f"R{34 + 2 * n}", "4.7k", f"V3V3_AON_{n}", f"SLOT{n}_SDA"),
              R(f"R{35 + 2 * n}", "4.7k", f"V3V3_AON_{n}", f"SLOT{n}_SCL"))
    exp_a = {"21": "GND", "2": "GND", "3": "GND", "22": "I2C_SCL", "23": "I2C_SDA", "24": core, "12": "GND",
             "1": "EXP_INT_N"}
    exp_b = {"21": core, "2": "GND", "3": "GND", "22": "I2C_SCL", "23": "I2C_SDA", "24": core, "12": "GND",
             "1": "EXP_INT_N"}
    p0 = ["4", "5", "6", "7", "8", "9", "10", "11"]
    p1 = ["13", "14", "15", "16", "17", "18", "19", "20"]
    for n in range(4):
        exp_a[p0[n]] = f"SLOT_SW_EN{n}"
        exp_a[p0[4 + n]] = f"SLOT_AON_EN{n}"
        exp_a[p1[n]] = f"SLOT{n}_RESET_N"
        exp_a[p1[4 + n]] = f"SLOT{n}_CS_N"
        exp_b[p0[n]] = f"SLOT{n}_PRESENT_N"
        exp_b[p0[4 + n]] = f"SLOT{n}_ALERT_N"
    for pin, net in zip(p1, ["UART_SEL0", "UART_SEL1", "UART_MUX_EN_N", "PRESENT_PROBE",
                             "CHG_INT_N", "CHG_PG_N", "PMUX_ST", "SW_FAULT_N"]):
        exp_b[pin] = net
    b.add(
        Part("U15", "Interface_Expansion:TCA9535PWR", "TCA9535PWR", exp_a, note="EXP_A root 0x20"),
        C("C22", "100n", core),
        Part("U16", "Interface_Expansion:TCA9535PWR", "TCA9535PWR", exp_b, note="EXP_B root 0x21"),
        C("C23", "100n", core),
        R("R42", "1M", "UART_SEL0", "GND"), R("R43", "1M", "UART_SEL1", "GND"),
        R("R44", "100k", core, "UART_MUX_EN_N", "mux disabled by default (S-3)"),
        R("R77", "1M", "PRESENT_PROBE", "GND", "probe idles low before the expander is configured"),
    )
    # EXP_C (L-03, HD-032): slot LEDs and the enclosure tamper loop. Unused pins: firmware drives them low.
    exp_c = {"21": "GND", "2": core, "3": "GND", "22": "I2C_SCL", "23": "I2C_SDA", "24": core, "12": "GND",
             "1": "EXP_INT_N"}
    for pin in p0 + p1:
        exp_c[pin] = None
    for n in range(4):
        exp_c[p0[n]] = f"SLOT_LED{n}_N"
    exp_c[p0[4]] = "TAMPER_N"
    b.add(Part("U18", "Interface_Expansion:TCA9535PWR", "TCA9535PWR", exp_c,
               note="EXP_C root 0x22: P00-P03 slot LEDs (active low), P04 TAMPER_N, rest unused (driven low)"),
          C("C37", "100n", core))
    for n in range(4):
        b.add(*LED(f"D{9 + n}", f"R{95 + n}", f"S{n}", core, f"SLOT_LED{n}_N", "2.2k",
                   note=f"S{n}: slot {n} state, firmware-driven, dark on battery (L-03)"))
    b.add(
        # Lid switch closed = loop to GND. Open lid or cut wire reads high and interrupts (HD-032).
        # 1M keeps the closed-loop cost at 3.3 uA.
        Part("J9", "Connector_Generic:Conn_01x02", "TAMPER loop", {"1": "TAMPER_N", "2": "GND"},
             "Connector_JST:JST_PH_S2B-PH-SM4-TB_1x02-1MP_P2.00mm_Horizontal",
             "enclosure lid switch, closed when the lid is shut (HD-032)"),
        R("R99", "1M", core, "TAMPER_N", "tamper loop pull-up: 3.3 uA while the lid is shut"),
        # 1M x 10 nF: tau 10 ms, ~12 ms to the TCA9535 VIH (0.7 VCC) after the lid opens (B7 in spice_sim.py)
        C("C38", "10n", "TAMPER_N", note="tamper RC: ESD and contact-bounce filter, tau 10 ms"),
        # HD-034: EXP_INT_N is active low; inverted it joins the slot WAKEs in one ext1 ANY_HIGH mask
        Part("U19", "74xGxx:74AUP1G04", "SN74AUP1G04DBVR",
             {"1": None, "2": "EXP_INT_N", "3": "GND", "4": "EXP_WAKE", "5": core},
             "Package_TO_SOT_SMD:SOT-23-5", "EXP_INT_N -> active-high EXP_WAKE on GPIO14 (HD-034)"),
        C("C36", "100n", core),
    )
    umux = {"5": "UART1_TX", "7": "UART1_RX", "12": "UART_SEL0", "16": "UART_SEL1",
            "15": "UART_MUX_EN_N", "13": "UART_MUX_EN_N", "14": core, "6": "GND"}
    for n, (s1, s2) in enumerate([("4", "8"), ("3", "9"), ("2", "10"), ("1", "11")]):
        umux[s1], umux[s2] = f"SLOT{n}_UTX_M", f"SLOT{n}_URX_M"
    b.add(Part("U17", "Analog_Switch:TS3A5017RSV", "TS3A5017RSVR", umux), C("C24", "100n", core))

    # Sheet E: four slot connectors with safe defaults and ESD
    for n in range(4):
        pins = {str(c): main_slot_net(n, role) for c, role in EDGE.items()}
        aon = f"V3V3_AON_{n}"
        b.add(
            Part(f"J{4 + n}", "Connector_Generic:Conn_02x10_Odd_Even", f"sensor_edge_v1 slot {n}", pins,
                 "talos:Samtec_HSEC8-110-01-S-D-EM2",
                 "HSEC8-110-01-S-D-EM2 (30 µin gold); land pattern from Samtec drawing, host board needs the locating slot (HD-002)"),
            R(f"R{45 + 5 * n}", "1M", f"SLOT_WAKE{n}", "GND", "empty slot never wakes"),
            R(f"R{46 + 5 * n}", "100k", aon, f"SLOT{n}_ALERT_N"),
            R(f"R{47 + 5 * n}", "100k", aon, f"SLOT{n}_CS_N"),
            R(f"R{48 + 5 * n}", "100k", aon, f"SLOT{n}_RESET_N"),
            R(f"R{49 + 5 * n}", "100k", "PRESENT_PROBE", f"SLOT{n}_PRESENT_N", "biased only while probing"),
            Part(f"D{4 + n}", "Power_Protection:PESD3V3L5UV", "PESD5V0L5UV",  # PESD3V3L5UV symbol: same pinout
                 {"1": f"SLOT_WAKE{n}", "3": f"SLOT{n}_SDA", "4": f"SLOT{n}_SCL", "5": f"SLOT{n}_ALERT_N",
                  "6": f"SLOT{n}_RESET_N", "2": "GND"}),
            # Owner decision 7: UART lines idle high from the slot's own AON (no backfeed) and the
            # 330R bounds contention and ESD current into the mux
            R(f"R{78 + 4 * n}", "330", f"SLOT{n}_UART_TX", f"SLOT{n}_UTX_M"),
            R(f"R{79 + 4 * n}", "330", f"SLOT{n}_UART_RX", f"SLOT{n}_URX_M"),
            R(f"R{80 + 4 * n}", "100k", aon, f"SLOT{n}_UART_TX"),
            R(f"R{81 + 4 * n}", "100k", aon, f"SLOT{n}_UART_RX"),
        )

    # Sheet F: factory test pads
    for i, net in enumerate([core, "GND", "CHIP_EN", "BOOT_N", "U0TXD", "U0RXD", "VBAT", "SYS_PATH",
                             "I2C_SDA", "I2C_SCL", "SW_FAULT_N", "USB_VBUS",
                             # bench §8 additions
                             "CHG_IN", "PV_IN", "PMUX_ST", "BAT_NTC", "CHG_INT_N", "CHG_PG_N", "EXP_INT_N",
                             "MUX_RESET_N", "USB_DP", "USB_DN", "V3V3_REG", "SYS_REG", "GND", "GND", "GND",
                             # D-02 slot rails, D-03 wake lines, shared SPI, battery sense, tamper
                             *(f"V3V3_AON_{n}" for n in range(4)), *(f"V3V3_SW_{n}" for n in range(4)),
                             *(f"SLOT_WAKE{n}" for n in range(4)), "SPI_SCK", "SPI_MOSI", "SPI_MISO",
                             "VBAT_SENSE", "TAMPER_N", "EXP_WAKE"]):
        b.add(TP(f"TP{i + 1}", net))
    return b


# ---- TL-SN-002 ------------------------------------------------------------------------
def sensor_node_v2() -> Board:
    """Self-contained sensor node (ADR 0019): radio, every sensor soldered on, 2 x AA primary cells or USB-C,
    debug lamps and three buttons on one board, assembled 100 % by JLCPCB. No slots, cards, mux or charger.
    Pins provisional (ADR 0013 rule); GPIO roles match firmware board_tl_sn_002.h."""
    b = Board("TL-SN-002", "TL-SN-002 Self-contained Sensor Node Rev A (generated, provisional)",
              # VSYS is driven by the TPS2116 (a power output); PIR_VDD comes through the R17/C18 filter
              external_power={"GND", "USB_VBUS", "BATT_RAW", "VBAT", "SYS_REG", "V3V3", "PIR_VDD"})
    v = "V3V3"
    mcu = {
        "3": v, "15": v, "8": "CHIP_EN", "9": "SPARE_IO0", "10": "VBAT_SENSE", "5": "I2C_SDA", "6": "I2C_SCL",
        "18": "USB_PRESENT", "19": "SPARE_IO5", "22": "STATUS_LED_N", "23": "BOOT_N", "20": "MAG_WAKE",
        "21": "ACC_INT", "16": "PIR_OUT", "12": "TAMPER", "13": "USER_BTN", "24": "SPARE_IO22",
        "25": "SPARE_IO25", "30": "U0RXD", "31": "U0TXD", "26": "MCU_USB_DN", "27": "MCU_USB_DP",
    }
    for n in ("4", "7", "17", "28", "29", "32", "33", "34", "35"):
        mcu[n] = None
    for n in ["1", "2", "11", "14"] + [str(i) for i in range(36, 54)]:
        mcu[n] = "GND"
    b.add(
        Part("U1", "talos:ESP32-H2-MINI-1", "ESP32-H2-MINI-1-H4S", mcu, None,
             "land pattern = datasheet Fig. 11; antenna keepout at board edge; JLCPCB global sourcing (HD-023)"),
        C("C1", "10u", v, bulk=True), C("C2", "100n", v),
        R("R1", "10k", v, "CHIP_EN", "EN RC per datasheet"), C("C3", "1u", "CHIP_EN"),
        Part("SW1", "Switch:SW_Push", "RESET", {"1": "CHIP_EN", "2": "GND"}, "Button_Switch_SMD:SW_SPST_PTS810"),
        R("R2", "10k", v, "BOOT_N", "GPIO9 strap pull-up (SPI boot)"),
        Part("SW2", "Switch:SW_Push", "BOOT", {"1": "BOOT_N", "2": "GND"}, "Button_Switch_SMD:SW_SPST_PTS810",
             "download mode at reset; service mode after boot"),
        # USER: active high so it shares the GPIO10-14 ext1 ANY_HIGH wake mask
        Part("SW3", "Switch:SW_Push", "USER", {"1": "USER_BTN", "2": v}, "Button_Switch_SMD:SW_SPST_PTS810",
             "short press: test report; long press: commissioning"),
        R("R3", "100k", "USER_BTN", "GND", "USER idles low"),
        R("R4", "10k", v, "STATUS_LED_N", "GPIO8 strap held high"),
        Part("D1", "Device:LED", "STATUS", {"2": v, "1": "LED_K"}, FP_LED, "STAT: firmware blink codes"),
        R("R5", "1k", "LED_K", "STATUS_LED_N"),
        R("R6", "4.7k", v, "I2C_SDA"), R("R7", "4.7k", v, "I2C_SCL"),
        Part("J10", "Connector_Generic:Conn_01x06", "TC2030 PROG",
             {"1": v, "2": "CHIP_EN", "3": "GND", "4": "U0TXD", "5": "BOOT_N", "6": "U0RXD"}, FP_TAG,
             "recovery console/flash: 1 3V3, 2 EN, 3 GND, 4 U0TXD, 5 BOOT, 6 U0RXD (USB-C is the primary path)"),
    )
    # USB-C: power, flashing, console and JTAG over the H2's built-in USB Serial/JTAG
    b.add(
        Part("J1", "Connector:USB_C_Receptacle_USB2.0_16P", "USB-C sink",
             {"A1": "GND", "B1": "GND", "A12": "GND", "B12": "GND", "SH": "GND",
              "A4": "USB_VBUS", "A9": "USB_VBUS", "B4": "USB_VBUS", "B9": "USB_VBUS",
              "A5": "USB_CC1", "B5": "USB_CC2", "A6": "USB_DP", "B6": "USB_DP",
              "A7": "USB_DN", "B7": "USB_DN", "A8": None, "B8": None},
             "Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal"),
        R("R10", "5.1k", "USB_CC1", "GND", "UFP Rd"), R("R11", "5.1k", "USB_CC2", "GND", "UFP Rd"),
        Part("U2", "Power_Protection:USBLC6-2SC6", "USBLC6-2SC6",
             {"1": "USB_DP", "6": "USB_DP", "3": "USB_DN", "4": "USB_DN", "5": "USB_VBUS", "2": "GND"}),
        C("C5", "4.7u", "USB_VBUS", bulk=True),
        R("R66", "0R", "MCU_USB_DP", "USB_DP"), R("R67", "0R", "MCU_USB_DN", "USB_DN"),
        C("C25", "DNP", "MCU_USB_DP", dnp=True, note="USB D+ shunt C option (Espressif)"),
        C("C26", "DNP", "MCU_USB_DN", dnp=True, note="USB D- shunt C option (Espressif)"),
        *LED("D8", "R94", "PWR", "USB_VBUS", "GND", "3.3k", note="PWR: USB present (hardware, ~0.9 mA from VBUS)"),
        # USB present: divided VBUS, zero battery current
        R("R12", "100k", "USB_VBUS", "USB_PRESENT", "VBUS/2 to GPIO4"), R("R13", "100k", "USB_PRESENT", "GND"),
    )
    # Power: 2 x AA primary (never charged) or USB, TPS2116 priority mux, TPS63900 buck-boost to 3.3 V
    b.add(
        Part("BT1", "Device:Battery", "2xAA Li-FeS2", {"1": "BATT_RAW", "2": "GND"},
             "Battery:BatteryHolder_Keystone_2462_2xAA",
             "Keystone 2462, 2 x AA lithium (Energizer L91 class) or alkaline; primary cells, never charged"),
        # reversed cells: body diode blocks, Vgs ~ 0 keeps the channel off (same pattern as TL-SN-001 Q1)
        Part("Q1", "Transistor_FET:AO3401A", "AO3401A", {"1": "BATFET_G", "2": "VBAT", "3": "BATT_RAW"},
             "Package_TO_SOT_SMD:SOT-23", "reverse-cell P-FET: D = cells, S = VBAT"),
        R("R69", "100k", "BATFET_G", "GND", "Q1 gate"),
        R("R8", "4.7M", "VBAT", "VBAT_SENSE"), R("R9", "4.7M", "VBAT_SENSE", "GND"), C("C4", "100n", "VBAT_SENSE"),
        Part("U3", "Power_Management:TPS2116DRL", "TPS2116DRLR",
             {"3": "USB_VBUS", "6": "VBAT", "2": "VSYS", "7": "VSYS", "1": "GND", "5": "USB_VBUS",
              "4": "PMUX_PR1", "8": None},
             note="priority mode (MODE = VIN1): USB above ~3.7 V wins, else the cells (SLVSFG1A 8.1); "
                  "VIN2 standby 50 nA typ"),
        R("R14", "27k", "USB_VBUS", "PMUX_PR1", "PR1 divider: 1.0 V VREF x 3.7 = 3.7 V switchover"),
        R("R15", "10k", "PMUX_PR1", "GND"),
        C("C6", "1u", "VBAT"), C("C7", "10u", "VSYS", bulk=True),
        Part("U5", "Regulator_Switching:TPS63900", "TPS63900DSKR",
             {"10": "SYS_REG", "1": "SYS_REG", "2": "GND", "3": "REG_CFG1", "4": "REG_CFG2",
              "5": "REG_CFG3", "6": "V3V3_REG", "9": "REG_LX1", "7": "REG_LX2", "8": "GND", "11": "GND"},
             "Package_SON:WSON-10-1EP_2.5x2.5mm_P0.5mm_EP1.2x2mm", "1.8-5.5 V in: AA pair to end of life, or VBUS"),
        R("R18", "36.5k 1%", "REG_CFG1", "GND", "SEL-high 3.3 V, unlimited ILIM (Table 7-2)"),
        R("R19", "0R", "REG_CFG2", "GND", "SEL-high 3.3 V, unlimited ILIM (Table 7-2)"),
        R("R20", "16.2k 1%", "REG_CFG3", "GND", "SEL-low 3.3 V (Table 7-3)"),
        Part("L1", "Device:L", "2.2u DFE252012F-2R2M", {"1": "REG_LX1", "2": "REG_LX2"},
             "Inductor_SMD:L_1008_2520Metric"),
        C("C11", "10u", "SYS_REG", bulk=True), C("C12", "22u", "V3V3_REG", bulk=True),
        *SPLIT("R70", "JP1", "VSYS", "SYS_REG", "source to regulator"),
        *SPLIT("R71", "JP2", "V3V3_REG", v, "regulator to 3.3 V loads"),
    )
    # Sensors, every board (ADR 0019): 3-axis Hall at an edge, accelerometer by a mounting hole,
    # temperature/humidity on a slotted thermal tongue, PIR (lens in the housing)
    b.add(
        Part("U6", "Sensor_Magnetic:TMAG5273x2QDBV", "TMAG5273A2QDBVT",
             {"1": "I2C_SCL", "2": "GND", "3": "GND", "4": v, "5": "MAG_INT_N", "6": "I2C_SDA"},
             note="3-axis Hall, A2 range; I2C address per datasheet (A = 0x35, VERIFY); volatile config"),
        C("C13", "100n", v), R("R16", "100k", v, "MAG_INT_N", "INT open-drain pull-up"),
        Part("U7", "74xGxx:74AUP1G04", "SN74AUP1G04DBVR",
             {"1": None, "2": "MAG_INT_N", "3": "GND", "4": "MAG_WAKE", "5": v}, "Package_TO_SOT_SMD:SOT-23-5",
             "INT_N to active-high MAG_WAKE (GPIO10, ext1 ANY_HIGH)"),
        C("C14", "100n", v),
        Part("U8", "talos:LIS2DW12", "LIS2DW12TR",
             {"9": v, "10": v, "1": "I2C_SCL", "4": "I2C_SDA", "2": v, "3": v, "12": "ACC_INT", "11": None,
              "5": "GND", "7": "GND", "6": "GND", "8": "GND"},
             note="CS high = I2C; SA0 high = 0x19; INT1 push-pull active high to GPIO11"),
        C("C15", "100n", v), C("C16", "1u", v),
        Part("U9", "Sensor_Humidity:SHT4x", "SHT40-AD1B", {"1": "I2C_SDA", "2": "I2C_SCL", "3": v, "4": "GND"},
             "talos:Sensirion_DFN-4_1.5x1.5mm_P0.8mm_SHT4x_CentralKeepout",
             "on the slotted thermal tongue; no copper under the die"),
        C("C17", "100n", v),
        Part("U10", "talos:EKMB", "PIR223D-S0", {"1": "PIR_VDD", "2": "PIR_OUT", "3": "GND"},
             "talos:PIR_NEWOPTO_PIR223D_TO-5",
             "NEWOPTO digital PIR, TO-5: 1 VDD, 2 REL, 3 VSS (spec 2020-06-01 p5; HD-044 checked); lens in the housing"),
        # 330 R keeps the expander pin under ~10 mA while C18 charges (spice B12; 100 R peaked at 30 mA)
        R("R17", "330", "PIR_PWR", "PIR_VDD", "PIR supply filter (expander pin, held in deep sleep)"),
        C("C18", "4.7u", "PIR_VDD", bulk=True, note="PIR supply filter"),
        R("R21", "1M", "PIR_OUT", "GND", "PIR output idles low when unpowered"),
    )
    # Lamps and the PIR rail on one TCA9535 (0x20); unused pins driven low by firmware
    exp = {"21": "GND", "2": "GND", "3": "GND", "22": "I2C_SCL", "23": "I2C_SDA", "24": v, "12": "GND", "1": None}
    p0 = ["4", "5", "6", "7", "8", "9", "10", "11"]
    p1 = ["13", "14", "15", "16", "17", "18", "19", "20"]
    roles = ["LINK", "MAG", "VIB", "PIR", "ENV", "TAMP", "BATT", "FLT"]
    for pin, role in zip(p0, roles):
        exp[pin] = f"LED_{role}_N"
    exp[p1[0]] = "PIR_PWR"
    exp[p1[1]] = "TAMPER_DOOR"
    for pin in p1[2:]:
        exp[pin] = None
    b.add(Part("U14", "Interface_Expansion:TCA9535PWR", "TCA9535PWR", exp,
               note="0x20: P00-P07 lamps (active low), P10 PIR_PWR, P11 TAMPER_DOOR in, P12-P17 unused (driven low)"),
          C("C21", "100n", v))
    for i, role in enumerate(roles):
        colour = "LED_RED" if role in ("BATT", "FLT") else "LED_GREEN"
        b.add(*LED(f"D{9 + i}", f"R{95 + i}", role, v, f"LED_{role}_N", "2.2k", colour=colour,
                   note=f"{role}: firmware lamp, dark on battery unless service mode"))
    b.add(
        # Tamper loop, all on the board (owner 2026-11-22: zero-touch, tool-free battery door that alarms).
        # V3V3 - R30 - TAMPER - SW4 (lid) - TAMPER_LOOP - JP3 (or J9's external switches) - TAMPER_DOOR
        # - SW5 (battery door) - GND. Housing spring fingers hold both switches pressed (closed) when shut.
        # TAMPER (the wake line) goes high if either opens. TAMPER_DOOR, read on EXP P11, is high only when
        # the battery door is open (lid open with the door shut reads low): firmware tells them apart.
        Part("SW4", "Switch:SW_Push", "LID", {"1": "TAMPER", "2": "TAMPER_LOOP"},
             "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A",
             "lid tamper: pressed (closed) by the front cover's spring finger (H-05)"),
        Part("SW5", "Switch:SW_Push", "BATT DOOR", {"1": "TAMPER_DOOR", "2": "GND"},
             "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A",
             "battery-door tamper: pressed (closed) by the tool-free door's spring finger (HD-049)"),
        Part("JP3", "Jumper:SolderJumper_2_Bridged", "LOOP", {"1": "TAMPER_LOOP", "2": "TAMPER_DOOR"},
             "Jumper:SolderJumper-2_P1.3mm_Bridged_RoundedPad1.0x1.5mm",
             "bridged: no external loop. Cut it and fit J9 to add external switches in series"),
        Part("J9", "Connector_Generic:Conn_01x02", "EXT LOOP", {"1": "TAMPER_LOOP", "2": "TAMPER_DOOR"},
             "Connector_JST:JST_PH_S2B-PH-SM4-TB_1x02-1MP_P2.00mm_Horizontal",
             "optional external switches (wall, extra door) in series; not fitted", dnp=True),
        R("R30", "1M", v, "TAMPER", "tamper pull-up: 3.3 uA while shut; high = open or cut"),
        R("R31", "4.7M", v, "TAMPER_DOOR", "door-switch sense pull-up: 0.7 uA while the door is shut"),
        C("C38", "10n", "TAMPER", note="tamper RC: tau 10 ms (spice B7)"),
    )
    for i, net in enumerate([v, "GND", "GND", "GND", "VSYS", "VBAT", "BATT_RAW", "USB_VBUS", "SYS_REG", "V3V3_REG",
                             "I2C_SDA", "I2C_SCL", "MAG_WAKE", "MAG_INT_N", "ACC_INT", "PIR_OUT", "PIR_VDD",
                             "TAMPER", "USER_BTN", "CHIP_EN", "BOOT_N", "U0TXD", "U0RXD", "USB_PRESENT",
                             "VBAT_SENSE", "USB_DP", "USB_DN", "SPARE_IO0", "SPARE_IO5", "SPARE_IO22",
                             "SPARE_IO25", "TAMPER_DOOR"]):
        b.add(TP(f"TP{i + 1}", net))
    return b


# ---- TL-DBG-001 -----------------------------------------------------------------------
def debug_adapter() -> Board:
    """One-slot USB debug adapter: a card plugs in beside it (coplanar) exactly as it does on the main
    board, and the host PC reaches it through an MCP2221A USB-to-I2C/UART bridge (no firmware).

    It mirrors one main-board slot (same socket, same rail switches, pull-ups and ESD), so a card that
    works here sees the same electrical environment as in TL-SN-001. Differences, by design:
    - Both rails default OFF (1M pull-downs); the host powers the card up when it is ready to watch.
    - JP1/JP2 put each card rail through a 2-pin header: pull the shunt and insert a current meter
      (Nordic PPK2 ammeter mode) to measure the card alone.
    - SPI is not bridged (the MCP2221A has none): SCK/MOSI/MISO/CS_N go to test pads for a logic analyser.
    - RESET_N is pulled up only (test pad); power-cycling AON is the reset.
    - PRESENT_N lights D1 when a card is seated (the card grounds contact 18).
    MCP2221A GPIO roles must be written to its flash once by the host tool, with no card seated. Factory
    defaults (DS20005565E §1.7.1, Registers 1-12..1-15): GP0 = LED_URx, idles high; GP2 = USBCFG, high once
    enumerated. So an unconfigured adapter turns both card rails on. After configuration the settings
    load from flash at power-up, and the 1M pull-downs hold the rails off until then. The 1k series
    resistors bound contention on GP1 (LED_UTx) and GP3 (LED_I2C) before configuration.
    """
    b = Board("TL-DBG-001", "TL-DBG-001 One-slot USB debug adapter Rev A (generated, provisional)",
              # V3V3_AON reaches the card side through the JP1 meter header (passive), so ERC needs a flag
              external_power={"GND", "USB_VBUS", "V3V3_AON"})
    v = "V3V3"
    socket = {}
    for c, r in EDGE.items():
        socket[str(c)] = {"GND": "GND", "AON": "V3V3_AON", "SW": "V3V3_SW", "V5_RSVD": None,
                          "ANALOG_RSVD": None}.get(r, r)
    b.add(
        # USB-C device port, same parts as the Sensor Node
        Part("J1", "Connector:USB_C_Receptacle_USB2.0_16P", "USB-C sink",
             {"A1": "GND", "B1": "GND", "A12": "GND", "B12": "GND", "SH": "GND",
              "A4": "USB_VBUS", "A9": "USB_VBUS", "B4": "USB_VBUS", "B9": "USB_VBUS",
              "A5": "USB_CC1", "B5": "USB_CC2", "A6": "USB_DP", "B6": "USB_DP",
              "A7": "USB_DN", "B7": "USB_DN", "A8": None, "B8": None},
             "Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal"),
        R("R1", "5.1k", "USB_CC1", "GND", "UFP Rd"), R("R2", "5.1k", "USB_CC2", "GND", "UFP Rd"),
        Part("U1", "Power_Protection:USBLC6-2SC6", "USBLC6-2SC6",
             {"1": "USB_DP", "6": "USB_DP", "3": "USB_DN", "4": "USB_DN", "5": "USB_VBUS", "2": "GND"}),
        # 3.3 V for the bridge and the card (AP2112K: 600 mA, EN tied to VIN)
        Part("U2", "Regulator_Linear:AP2112K-3.3", "AP2112K-3.3TRG1",
             {"1": "USB_VBUS", "2": "GND", "3": "USB_VBUS", "4": None, "5": v}),
        C("C1", "1u", "USB_VBUS"), C("C2", "10u", v, bulk=True),
        # USB bridge. VDD = 3.3 V, so VUSB ties to VDD (DS20005565E §1.6.2.3); RST has an internal pull-up
        Part("U3", "Interface_USB:MCP2221AxSL", "MCP2221AT-I/SL",
             {"1": v, "11": v, "14": "GND", "4": "BRIDGE_RST_N", "12": "USB_DN", "13": "USB_DP",
              "9": "SDA", "10": "SCL", "6": "BRIDGE_TX", "5": "BRIDGE_RX",
              "2": "DBG_AON_EN", "7": "DBG_SW_EN", "3": "BRIDGE_WAKE", "8": "BRIDGE_ALERT"},
             note="GP0 AON enable, GP2 SW enable, GP1 WAKE in (interrupt-on-change), GP3 ALERT_N in"),
        C("C3", "100n", v), C("C4", "1u", v),
        R("R3", "10k", v, "BRIDGE_RST_N"),
        # card rails: one slot's worth of the main board's switches, both default off
        Part("U4", "Power_Management:TPS22917DBV", "TPS22917DBVR",
             {"1": v, "2": "GND", "3": "DBG_AON_EN", "4": "AON_CT", "5": "AON_QOD", "6": "AON_METER"},
             note="card AON switch, default off on the adapter"),
        R("R4", "1M", "DBG_AON_EN", "GND"),
        R("R17", "0R", "AON_QOD", "AON_METER", "QOD to VOUT: card AON decays fast between power cycles"),
        C("C8", "DNP", "AON_CT", v, dnp=True, note="TPS22917 CT option, to VIN (SLVSDW8B 9.3.2)"),
        Part("JP1", "Connector_Generic:Conn_01x02", "AON meter", {"1": "AON_METER", "2": "V3V3_AON"},
             "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical",
             "shunt fitted for normal use; remove and insert a PPK2 (ammeter) to measure card AON current"),
        C("C5", "1u", "V3V3_AON"),
        # Same current-limited SW switch as the main board's slots (owner decision 3)
        Part("U5", "talos:TPS2553", "TPS2553DBVR-1",
             {"1": v, "2": "GND", "3": "DBG_SW_EN", "4": "DBG_SW_FAULT_N", "5": "SW_ILIM", "6": "SW_METER"},
             note="card switched rail, default off, latch-off current limit"),
        R("R5", "1M", "DBG_SW_EN", "GND"),
        R("R6", "150k 1%", "SW_ILIM", "GND", "IOS 155-207 mA (SLVS841G)"),
        R("R18", "100k", v, "DBG_SW_FAULT_N", "FAULT_N pull-up (test pad; no spare bridge GPIO)"),
        R("R19", "100k", "SW_METER", "GND", "SW bleed: TPS2553 has no output discharge"),
        C("C9", "100n", v),
        Part("JP2", "Connector_Generic:Conn_01x02", "SW meter", {"1": "SW_METER", "2": "V3V3_SW"},
             "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical",
             "shunt fitted for normal use; remove and insert a PPK2 (ammeter) to measure card SW current"),
        C("C6", "1u", "V3V3_SW"),
        # the slot, with the main board's defaults (pull-ups on the card's own AON rail: no back-powering)
        Part("J2", "Connector_Generic:Conn_02x10_Odd_Even", "sensor_edge_v1 slot (debug)", socket,
             "talos:Samtec_HSEC8-110-01-S-D-EM2",
             "HSEC8-110-01-S-D-EM2 (30 µin gold); land pattern from Samtec drawing, host board needs the locating slot (HD-002)"),
        R("R7", "4.7k", "V3V3_AON", "SDA"), R("R8", "4.7k", "V3V3_AON", "SCL"),
        R("R9", "1M", "WAKE", "GND", "empty slot never wakes"),
        R("R10", "100k", "V3V3_AON", "ALERT_N"),
        R("R11", "100k", "V3V3_AON", "CS_N"),
        R("R12", "100k", "V3V3_AON", "RESET_N"),
        R("R13", "100k", "V3V3_AON", "UART_RX", "idle-high when the card has no UART"),
        R("R20", "100k", "V3V3_AON", "UART_TX", "idle-high, as on the main board (decision 7)"),
        R("R21", "100k", "SCK", "GND", "SPI is not bridged: hold it quiet"),
        R("R22", "100k", "MOSI", "GND", "SPI is not bridged: hold it quiet"),
        Part("D2", "Power_Protection:PESD3V3L5UV", "PESD5V0L5UV",  # PESD3V3L5UV symbol: same pinout
             {"1": "WAKE", "3": "SDA", "4": "SCL", "5": "ALERT_N", "6": "RESET_N", "2": "GND"}),
        # card outputs reach the bridge through 1k: bounds contention while GP pins are unconfigured
        R("R14", "1k", "WAKE", "BRIDGE_WAKE"), R("R15", "1k", "ALERT_N", "BRIDGE_ALERT"),
        # bridge TX is buffered from the card's AON rail: Ioff keeps an unpowered card unpowered
        Part("U6", "74xGxx:74LVC1G125", "SN74LVC1G125DBVR",
             {"1": "GND", "2": "BRIDGE_TX", "3": "GND", "4": "DBG_UTX", "5": "AON_METER"},
             "Package_TO_SOT_SMD:SOT-23-5",
             "OE_N tied low; powered from the card's switched rail ahead of JP1, so its current isn't metered"),
        C("C7", "100n", "AON_METER"),
        R("R23", "330", "DBG_UTX", "UART_TX", "series R as on the main board (decision 7)"),
        R("R24", "330", "UART_RX", "BRIDGE_RX", "series R as on the main board (decision 7)"),
        # card-seated indicator
        Part("D1", "Device:LED", "STATUS", {"2": v, "1": "SEATED_K"}, "LED_SMD:LED_0603_1608Metric",
             "lit when a card grounds PRESENT_N"),
        R("R16", "1k", "SEATED_K", "PRESENT_N"),
        # L-07..L-09: bridge-side lamps; none sits on a metered rail, so PPK2 numbers are unaffected
        *LED("D3", "R25", "PWR", v, "GND", "2.2k", note="PWR: adapter 3.3 V up"),
        *LED("D4", "R26", "AON", "DBG_AON_EN", "GND", "2.2k",
             note="AON: host asked for the card AON rail (lit from the MCP2221 GP0 drive, ~0.5 mA)"),
        *LED("D5", "R27", "SW", "DBG_SW_EN", "GND", "2.2k",
             note="SW: host asked for the card SW rail (lit from the MCP2221 GP2 drive, ~0.5 mA)"),
        *LED("D6", "R28", "FLT", v, "DBG_SW_FAULT_N", "2.2k", colour="LED_RED",
             note="FLT: TPS2553 current-limit trip"),
    )
    for i, net in enumerate([v, "GND", "V3V3_AON", "V3V3_SW", "SCK", "MOSI", "MISO", "CS_N", "RESET_N",
                             "UART_TX", "UART_RX", "SDA", "SCL", "WAKE", "ALERT_N", "GND",
                             "BRIDGE_TX", "USB_VBUS", "DBG_SW_FAULT_N"]):
        b.add(TP(f"TP{i + 1}", net))
    # D-07: two scope-ground loops (THT), clip-on rather than 1 mm pads
    for i in range(2):
        b.add(Part(f"TP{20 + i}", "Connector:TestPoint", "GND hook", {"1": "GND"}, FP_HOOK,
                   "Keystone 5001 miniature loop for scope/meter ground clips"))
    return b


# ---- cards ----------------------------------------------------------------------------
def card(name: str, title: str, used: set[str], parts: list[Part], pads: tuple[str, ...] = ()) -> Board:
    b = Board(name, title, external_power={"GND", "V3V3_AON"})
    b.add(card_edge(used), *id_eeprom())
    b.add(*parts)
    b.add(TP("TP1", "V3V3_AON"), TP("TP2", "GND"))
    b.add(*(TP(f"TP{3 + i}", net) for i, net in enumerate(pads)))
    return b


def boards() -> list[Board]:
    i2c = {"AON", "SDA", "SCL"}
    out = [sensor_node(), sensor_node_v2()]
    # 3-axis Hall so the card works in any orientation (HD-020). TMAG5273 INT is open-drain active-low
    # and latched until the host addresses the device; the inverter turns it into the slot standard's
    # push-pull, idle-low WAKE (SLYS045C Table 4-1, §6.3.3; SCES571K pin table).
    out.append(card("TL-MOD-001", "TL-MOD-001 Magnetic contact card Rev A", i2c | {"WAKE"}, [
        Part("U2", "Sensor_Magnetic:TMAG5273x1QDBV", "TMAG5273A1QDBVR",
             {"1": "SCL", "2": "GND", "3": "GND", "4": "V3V3_AON", "5": "MAG_INT_N", "6": "SDA"},
             note="I2C 0x35 (A1), +/-40/80 mT; TEST pin 3 to GND; volatile config, host sets W&S mode"),
        C("C2", "100n", "V3V3_AON"),
        R("R1", "100k", "V3V3_AON", "MAG_INT_N", "INT pull-up (open-drain)"),
        Part("U3", "74xGxx:74AUP1G04", "SN74AUP1G04DBVR",
             {"1": None, "2": "MAG_INT_N", "3": "GND", "4": "WAKE", "5": "V3V3_AON"},
             "Package_TO_SOT_SMD:SOT-23-5", "inverts INT_N to idle-low push-pull WAKE"),
        C("C3", "100n", "V3V3_AON"),
    ], pads=("MAG_INT_N",)))
    out.append(card("TL-MOD-002", "TL-MOD-002 PIR card Rev A", i2c | {"WAKE"}, [
        Part("U2", "talos:EKMB", "EKMB1101111", {"1": "V3V3_AON", "2": "WAKE", "3": "GND"}, None,
             "TO-5 3-pin on PCD 5.08 mm; pin positions from bottom-view drawing, verify"),
        C("C2", "100n", "V3V3_AON"),
    ], pads=("WAKE",)))
    out.append(card("TL-MOD-003", "TL-MOD-003 Vibration/impact card Rev A", i2c | {"WAKE"}, [
        Part("U2", "talos:LIS2DW12", "LIS2DW12TR",
             {"9": "V3V3_AON", "10": "V3V3_AON", "1": "SCL", "4": "SDA", "2": "V3V3_AON", "3": "V3V3_AON",
              "12": "WAKE", "11": None, "5": "GND", "7": "GND", "6": "GND", "8": "GND"},
             note="CS high = I2C; SA0 high = 0x19 (internal pull-up cannot be disabled)"),
        C("C2", "100n", "V3V3_AON"), C("C3", "10u", "V3V3_AON", bulk=True),
    ], pads=("WAKE",)))
    out.append(card("TL-MOD-004", "TL-MOD-004 Temperature/humidity card Rev A", i2c, [
        Part("U2", "Sensor_Humidity:SHT4x", "SHT40-AD1B", {"1": "SDA", "2": "SCL", "3": "V3V3_AON", "4": "GND"},
             "talos:Sensirion_DFN-4_1.5x1.5mm_P0.8mm_SHT4x_CentralKeepout", "no copper under the sensor die"),
        C("C2", "100n", "V3V3_AON"),
    ]))
    used = {"AON", "SW", "SDA", "SCL", "WAKE", "ALERT_N", "SCK", "MOSI", "MISO", "CS_N", "UART_TX",
            "UART_RX", "RESET_N"}
    exp = {"21": "V3V3_AON", "2": "V3V3_AON", "3": "V3V3_AON", "22": "SCL", "23": "SDA", "24": "V3V3_AON",
           "12": "GND", "1": None,
           "4": "DRV_WAKE", "5": "DRV_ALERT", "6": "DRV_MISO", "7": "DRV_UART_RX",
           "8": "SW_SENSE", "9": "EXP_NC0", "10": "EXP_NC1", "11": "EXP_NC2",
           "13": "CS_N", "14": "RESET_N", "15": "SCK", "16": "MOSI", "17": "UART_TX",
           "18": "EXP_NC3", "19": "EXP_NC4", "20": "EXP_NC5"}
    out.append(card("TL-TEST-001", "TL-TEST-001 Slot test cartridge Rev A", used, [
        Part("U2", "Interface_Expansion:TCA9535PWR", "TCA9535PWR", exp, note="branch address 0x27"),
        C("C2", "100n", "V3V3_AON"),
        R("R1", "1k", "DRV_WAKE", "WAKE"), R("R2", "1k", "DRV_ALERT", "ALERT_N", "test only; ALERT_N is open-drain"),
        R("R3", "1k", "DRV_MISO", "MISO"), R("R4", "1k", "DRV_UART_RX", "UART_RX"),
        R("R5", "330", "V3V3_SW", "GND", "~10 mA switched-rail load"),
        # L-11: lamp on the switched rail; adds ~0.5 mA to the 10 mA load
        *LED("D1", "R20", "SW", "V3V3_SW", "GND", "2.2k", note="SW: slot switched rail on"),
        # At-speed loopbacks (owner decision 9): weak enough that an expander drive through 1k wins
        R("R6", "10k", "UART_TX", "UART_RX", "UART loopback"),
        R("R7", "10k", "MOSI", "MISO", "SPI loopback"),
        R("R8", "10k", "V3V3_SW", "SW_SENSE", "SW rail sense into P04"),
        # Defined levels so the test can tell a host pull-up from an open contact
        R("R9", "100k", "SCK", "GND"), R("R10", "100k", "MOSI", "GND"), R("R11", "100k", "UART_TX", "GND"),
        R("R12", "1M", "CS_N", "GND"), R("R13", "1M", "RESET_N", "GND"),
        *(R(f"R{14 + i}", "10k", f"EXP_NC{i}", "GND", "unused expander input held low") for i in range(6)),
    ], pads=("V3V3_SW", "WAKE", "SDA", "SCL", "ALERT_N")))
    out.append(debug_adapter())
    return out


# ---- schematic writer -----------------------------------------------------------------
def uid(*parts: str) -> str:
    return str(uuid.uuid5(NS, "/".join(parts)))


def snap(v: float) -> float:
    return round(round(v / G) * G, 4)


def effects(justify: str | None = None, hide: bool = False) -> list:
    e = ["effects", ["font", ["size", "1.27", "1.27"]]]
    if justify:
        e.append(["justify"] + justify.split())
    if hide:
        e.append(["hide", "yes"])
    return e


def place(board: Board, root: str, p: Part, sym, unit: int, upins, sx: float, sy: float) -> list:
    """One symbol unit plus a wire stub and label (or no-connect flag) on each of its pins."""
    items: list = []
    tag = p.ref if unit == 1 else f"{p.ref}#u{unit}"
    is_flag = p.ref.startswith("#")
    # test pads and Tag-Connect pads are bare copper, not purchased (the THT ground hooks are)
    in_bom = not is_flag and (p.footprint or "") not in BOARD_FEATURES
    props = [
        ["property", q("Reference"), q(p.ref), ["at", f"{sx:g}", f"{sy - 3 * G:g}", "0"], effects(hide=is_flag)],
        ["property", q("Value"), q(p.value), ["at", f"{sx:g}", f"{sy + 3 * G:g}", "0"], effects()],
        ["property", q("Footprint"), q(p.footprint if p.footprint is not None else sym.prop("Footprint")),
         ["at", f"{sx:g}", f"{sy:g}", "0"], effects(hide=True)],
        ["property", q("Datasheet"), q(sym.prop("Datasheet") or "~"), ["at", f"{sx:g}", f"{sy:g}", "0"],
         effects(hide=True)],
    ]
    if p.note:
        props.append(["property", q("Talos_Note"), q(p.note), ["at", f"{sx:g}", f"{sy:g}", "0"],
                      effects(hide=True)])
    inst = ["symbol", ["lib_id", q(p.lib_id)], ["at", f"{sx:g}", f"{sy:g}", "0"], ["unit", str(unit)],
            ["exclude_from_sim", "no"], ["in_bom", "yes" if in_bom else "no"],
            ["on_board", "no" if is_flag else "yes"], ["dnp", "yes" if p.dnp else "no"],
            ["uuid", q(uid(board.name, tag))], *props]
    for pin in upins:
        inst.append(["pin", q(pin.number), ["uuid", q(uid(board.name, p.ref, "pin", pin.number))]])
    inst.append(["instances", ["project", q(board.name), ["path", q(f"/{root}"),
                                                           ["reference", q(p.ref)], ["unit", str(unit)]]]])
    items.append(inst)

    seen = set()
    for pin in upins:
        net = p.pins[pin.number]
        ex, ey = pin_end(sx, sy, pin)
        key = (ex, ey, net)
        if key in seen:
            continue
        seen.add(key)
        if net is None:
            items.append(["no_connect", ["at", f"{ex:g}", f"{ey:g}"],
                          ["uuid", q(uid(board.name, p.ref, pin.number, "nc"))]])
            continue
        dx, dy = stub_dir(pin)
        lx, ly = round(ex + dx * G, 4), round(ey + dy * G, 4)
        items.append(["wire", ["pts", ["xy", f"{ex:g}", f"{ey:g}"], ["xy", f"{lx:g}", f"{ly:g}"]],
                      ["stroke", ["width", "0"], ["type", "default"]],
                      ["uuid", q(uid(board.name, p.ref, pin.number, "w"))]])
        angle, just = {(-1, 0): ("180", "right bottom"), (1, 0): ("0", "left bottom"),
                       (0, -1): ("90", "left bottom"), (0, 1): ("270", "right bottom")}[(dx, dy)]
        items.append(["label", q(net), ["at", f"{lx:g}", f"{ly:g}", angle], effects(just),
                      ["uuid", q(uid(board.name, p.ref, pin.number, "l"))]])
    return items



def _netclass(name: str, width: float) -> dict:
    return {"name": name, "clearance": 0.15, "track_width": width, "via_diameter": 0.65, "via_drill": 0.3,
            "microvia_diameter": 0.3, "microvia_drill": 0.1, "diff_pair_gap": 0.2, "diff_pair_width": 0.2,
            "diff_pair_via_gap": 0.25, "wire_width": 6, "bus_width": 12, "line_style": 0, "priority": 0,
            "pcb_color": "rgba(0, 0, 0, 0.000)", "schematic_color": "rgba(0, 0, 0, 0.000)"}


# Vias are 0.65/0.3 mm so two vias at 0.15 mm copper clearance keep JLCPCB's 0.5 mm hole-to-hole (HD-039).


def project_file(name: str) -> dict:
    """Project file with fab-oriented design rules (typical 4-layer/2-layer prototype service limits)."""
    return {
        "meta": {"filename": f"{name}.kicad_pro", "version": 3},
        "board": {"design_settings": {"rules": {
            "min_clearance": 0.15, "min_track_width": 0.1, "min_via_diameter": 0.5, "min_via_annular_width": 0.1,
            "min_through_hole_diameter": 0.3, "min_hole_clearance": 0.15, "min_hole_to_hole": 0.5,
            "min_copper_edge_clearance": 0.3, "min_silk_clearance": 0.0, "min_resolved_spokes": 1,
            "max_error": 0.005}}},
        "net_settings": {"classes": [{
            "name": "Default", "clearance": 0.15, "track_width": 0.15, "via_diameter": 0.65, "via_drill": 0.3,
            "microvia_diameter": 0.3, "microvia_drill": 0.1, "diff_pair_gap": 0.2, "diff_pair_width": 0.2,
            "diff_pair_via_gap": 0.25, "wire_width": 6, "bus_width": 12, "line_style": 0, "priority": 2147483647,
            "pcb_color": "rgba(0, 0, 0, 0.000)", "schematic_color": "rgba(0, 0, 0, 0.000)"},
            _netclass("Power", 0.3), _netclass("Rail", 0.25)],
            # Power: charger/regulator path (<=1 A). Rail: 3V3 distribution (<=0.35 A peaks) and VBAT, whose
            # BQ25186 pin sits mid-row at 0.4 mm pitch (0.3 mm there leaves no router margin). GND is the
            # In1 plane and V3V3_CORE the In2 plane on the 4-layer board.
            "netclass_patterns": [{"netclass": "Power", "pattern": pat} for pat in (
                "/SYS_PATH", "/SYS_REG", "/USB_VBUS", "/PMUX_IN1", "/CHG_IN", "/PV_IN", "/PV_RAW", "/REG_LX1",
                "/REG_LX2")]
            + [{"netclass": "Rail", "pattern": pat} for pat in ("/V3V3_*", "/VBAT", "/VBAT_PACK")],
            "meta": {"version": 4}},
        "schematic": {"page_layout_descr_file": ""},
    }


def write_board(board: Board, libs: SymbolLibraries) -> dict:
    paper, page_w = ("A1", 800.0) if len(board.parts) > 40 else ("A3", 400.0)
    root = uid(board.name, "root")
    syms = {p.lib_id: libs.get(p.lib_id) for p in board.parts}

    # One part per reference: a duplicate silently drops a part from intended-netlist.json (and the PCB).
    seen_refs: set[str] = set()
    for p in board.parts:
        if p.ref in seen_refs:
            raise SystemExit(f"{board.name}: duplicate reference {p.ref}")
        seen_refs.add(p.ref)

    # Pin coverage: every symbol pin must be assigned (net or explicit None).
    for p in board.parts:
        numbers = {pin.number for pin in syms[p.lib_id].pins}
        missing, extra = numbers - set(p.pins), set(p.pins) - numbers
        if missing or extra:
            raise SystemExit(f"{board.name} {p.ref}: unassigned pins {sorted(missing)}, unknown {sorted(extra)}")

    # Power flags for nets fed from connectors/batteries.
    flag_parts = []
    for i, net in enumerate(sorted(board.external_power)):
        flag_parts.append(Part(f"#FLG{i + 1:02d}", "power:PWR_FLAG", "PWR_FLAG", {"1": net}))
    syms["power:PWR_FLAG"] = libs.get("power:PWR_FLAG")
    all_parts = board.parts + flag_parts

    # Single-pin nets are almost always a typo.
    members: dict[str, list[str]] = {}
    for p in all_parts:
        for num, net in p.pins.items():
            if net is not None:
                members.setdefault(net, []).append(f"{p.ref}.{num}")
    lonely = {n: m for n, m in members.items() if len(m) < 2}
    if lonely:
        raise SystemExit(f"{board.name}: single-pin nets {lonely}")

    items: list = []
    margin = 20.0
    x_cursor, y_cursor, row_h = margin, 25.0, 0.0
    for p in all_parts:
        sym = syms[p.lib_id]
        units = sorted({pin.unit for pin in sym.pins}) or [1]
        for unit in units:
            upins = [pin for pin in sym.pins if pin.unit == unit]
            xs = [pin_end(0, 0, pin)[0] for pin in upins] or [0.0]
            ys = [pin_end(0, 0, pin)[1] for pin in upins] or [0.0]
            space = {}
            for d in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                names = [p.pins[pin.number] or "" for pin in upins if stub_dir(pin) == d]
                space[d] = (max(len(n) for n in names) * 1.0 + 3 * G) if names else 2 * G
            w = (max(xs) - min(xs)) + space[(-1, 0)] + space[(1, 0)] + 2 * G
            h = (max(ys) - min(ys)) + space[(0, -1)] + space[(0, 1)] + 4 * G
            if x_cursor + w > page_w:
                x_cursor, y_cursor, row_h = margin, y_cursor + row_h, 0.0
            sx = snap(x_cursor - min(xs) + space[(-1, 0)] + G)
            sy = snap(y_cursor - min(ys) + space[(0, -1)] + 2 * G)
            x_cursor += w
            row_h = max(row_h, h)
            items.extend(place(board, root, p, sym, unit, upins, sx, sy))

    lib_symbols = ["lib_symbols"] + [s.node for _, s in sorted(syms.items())]
    sch = ["kicad_sch", ["version", "20250114"], ["generator", q("talos_gen")], ["generator_version", q("1.0")],
           ["uuid", q(root)], ["paper", q(paper)],
           ["title_block", ["title", q(board.title)], ["rev", q("A")], ["company", q("Talos (open source)")],
            ["comment", "1", q("GENERATED by hardware/scripts/gen_schematics.py - do not edit by hand")],
            ["comment", "2", q("PROVISIONAL: not reviewed by an electrical engineer, not laid out, not built")]],
           lib_symbols, *items,
           ["sheet_instances", ["path", q("/"), ["page", q("1")]]], ["embedded_fonts", "no"]]

    out_dir = KICAD_DIR / board.name
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / f"{board.name}.kicad_sch").write_text(dump(sch) + "\n", encoding="utf-8")
    (out_dir / f"{board.name}.kicad_pro").write_text(json.dumps(project_file(board.name), indent=2) + "\n",
                                                       encoding="utf-8")
    libs_used = sorted({lib.split(":")[0] for lib in syms} - {"talos"})
    sym_table = ["sym_lib_table", ["version", "7"],
                 ["lib", ["name", q("talos")], ["type", q("KiCad")],
                  ["uri", q("${KIPRJMOD}/../libraries/talos-symbols.kicad_sym")], ["options", q("")],
                  ["descr", q("Talos project symbols (generated)")]]]
    for lib in libs_used:
        sym_table.append(["lib", ["name", q(lib)], ["type", q("KiCad")],
                          ["uri", q(f"${{KICAD10_SYMBOL_DIR}}/{lib}.kicad_sym")], ["options", q("")], ["descr", q("")]])
    (out_dir / "sym-lib-table").write_text(dump(sym_table) + "\n", encoding="utf-8")
    fp_libs = sorted({(p.footprint if p.footprint is not None else syms[p.lib_id].prop("Footprint")).split(":")[0]
                      for p in board.parts} - {""})
    if board.name in ("TL-SN-001", "TL-SN-002"):  # board-only fiducials (gen_pcbs.py) from the stock library
        fp_libs = sorted(set(fp_libs) | {"Fiducial"})
    if board.name in ("TL-SN-001", "TL-SN-002", "TL-DBG-001"):  # board-only mounting holes (gen_pcbs.py)
        fp_libs = sorted(set(fp_libs) | {"MountingHole"})
    fp_table = ["fp_lib_table", ["version", "7"]]
    for lib in fp_libs:
        uri = ("${KIPRJMOD}/../libraries/talos-footprints.pretty" if lib == "talos"
               else f"${{KICAD10_FOOTPRINT_DIR}}/{lib}.pretty")
        fp_table.append(["lib", ["name", q(lib)], ["type", q("KiCad")], ["uri", q(uri)], ["options", q("")],
                         ["descr", q("")]])
    (out_dir / "fp-lib-table").write_text(dump(fp_table) + "\n", encoding="utf-8")

    intended = {
        "board": board.name,
        "parts": {p.ref: {"lib_id": p.lib_id, "value": p.value,
                          "footprint": p.footprint if p.footprint is not None else syms[p.lib_id].prop("Footprint"),
                          "note": p.note, "pins": p.pins, "dnp": p.dnp,
                          "datasheet": syms[p.lib_id].prop("Datasheet") or "~",
                          "pin_names": {pin.number: pin.name for pin in syms[p.lib_id].pins}} for p in board.parts},
    }
    (out_dir / "intended-netlist.json").write_text(json.dumps(intended, indent=1, sort_keys=True) + "\n")
    return {"board": board.name, "parts": len(board.parts), "nets": len(members)}


def main() -> int:
    LIB_DIR.mkdir(parents=True, exist_ok=True)
    TALOS_LIB.write_text(dump(library_node()) + "\n", encoding="utf-8")
    fps = write_library(LIB_DIR / "talos-footprints.pretty")
    libs = SymbolLibraries(STOCK, {"talos": str(TALOS_LIB)})
    from boards import ACTIVE
    for b in boards():
        if b.name not in ACTIVE and "--archived" not in sys.argv:
            continue  # frozen in hardware/archive/modular-2026-11; --archived regenerates into hardware/kicad
        info = write_board(b, libs)
        print(f"{info['board']}: {info['parts']} parts, {info['nets']} nets")
    print(f"symbols: {', '.join(SYMBOLS)} -> {TALOS_LIB.relative_to(ROOT)}")
    print(f"footprints: {', '.join(fps)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
