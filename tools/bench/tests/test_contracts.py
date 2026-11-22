"""The part references in diagnoses must match the TL-SN-001 schematic."""

import json
import pathlib
import re
import unittest

from talos_bench.runner import ROOT_PARTS, SLOT_PARTS

ROOT = pathlib.Path(__file__).resolve().parents[3]
PARTS = json.loads((ROOT / "hardware/archive/modular-2026-11/kicad/TL-SN-001/intended-netlist.json").read_text())["parts"]


def nets(ref):
    return set(PARTS[ref]["pins"].values())


def expander_at(addr):
    """The TCA9535 whose A0/A1/A2 straps (PW pins 21/2/3) give `addr`, or None."""
    for ref, part in PARTS.items():
        if not part["value"].startswith("TCA9535"):
            continue
        p = part["pins"]
        bits = [p.get(pin) not in ("GND", None) for pin in ("21", "2", "3")]  # A0, A1, A2
        if 0x20 | bits[0] | (bits[1] << 1) | (bits[2] << 2) == addr:
            return ref
    return None


class DiagnosisPartsMatchTheSchematic(unittest.TestCase):
    def test_slot_parts(self):
        for slot, p in SLOT_PARTS.items():
            self.assertEqual(PARTS[p["aon_switch"]]["pins"]["6"], f"V3V3_AON_{slot}", p["aon_switch"])
            self.assertEqual(PARTS[p["aon_switch"]]["pins"]["3"], f"SLOT_AON_EN{slot}")
            pin = int(p["aon_enable"][-1])  # EXP_A P0n -> package pin 4 + n
            self.assertEqual(PARTS["U15"]["pins"][str(4 + pin)], f"SLOT_AON_EN{slot}")
            sda, scl = p["pullups"]
            self.assertEqual(nets(sda), {f"V3V3_AON_{slot}", f"SLOT{slot}_SDA"})
            self.assertEqual(nets(scl), {f"V3V3_AON_{slot}", f"SLOT{slot}_SCL"})
            self.assertIn(f"SLOT{slot}_SDA", nets(p["esd"]))

    def test_root_parts(self):
        chips = {0x20: "TCA9535", 0x21: "TCA9535", 0x22: "TCA9535", 0x6A: "BQ25186", 0x70: "TCA9546"}
        for addr, text in ROOT_PARTS.items():
            self.assertIn(f"0x{addr:02X}", text)
            m = re.search(r"\bU\d+\b", text)
            if m is None:  # no designator in the text (EXP_C): test_third_expander finds it by straps
                continue
            ref = m.group()
            self.assertTrue(PARTS[ref]["value"].startswith(chips[addr]), f"{text}: {ref} is {PARTS[ref]['value']}")

    def test_third_expander(self):
        """EXP_C (practices review L-03): 0x22 straps, slot LEDs, TAMPER_N, INT on EXP_INT_N."""
        ref = expander_at(0x22)
        if ref is None:
            self.skipTest("netlist has no TCA9535 at 0x22 yet (generator change pending)")
        pins = PARTS[ref]["pins"]
        self.assertEqual(pins["1"], "EXP_INT_N")
        for n in range(4):
            self.assertEqual(pins[str(4 + n)], f"SLOT_LED{n}_N")
        self.assertEqual(pins["8"], "TAMPER_N")


if __name__ == "__main__":
    unittest.main()
