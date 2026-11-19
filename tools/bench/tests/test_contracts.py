"""The part references in diagnoses must match the TL-SN-001 schematic."""

import json
import pathlib
import re
import unittest

from talos_bench.runner import ROOT_PARTS, SLOT_PARTS

ROOT = pathlib.Path(__file__).resolve().parents[3]
PARTS = json.loads((ROOT / "hardware/kicad/TL-SN-001/intended-netlist.json").read_text())["parts"]


def nets(ref):
    return set(PARTS[ref]["pins"].values())


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
        chips = {0x20: "TCA9535", 0x21: "TCA9535", 0x6A: "BQ25186", 0x70: "TCA9546"}
        for addr, text in ROOT_PARTS.items():
            ref = re.search(r"\bU\d+\b", text).group()
            self.assertTrue(PARTS[ref]["value"].startswith(chips[addr]), f"{text}: {ref} is {PARTS[ref]['value']}")
            self.assertIn(f"0x{addr:02X}", text)


if __name__ == "__main__":
    unittest.main()
