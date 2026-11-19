"""The tool's tables must match the schematic and the firmware registry."""

import json
import pathlib
import re
import unittest

from talos_dbg.adapter import MANIFEST, Manifest
from talos_dbg.modules import CARDS, MODULES

ROOT = pathlib.Path(__file__).resolve().parents[3]


class AdapterManifestMatchesSchematic(unittest.TestCase):
    def test_gp_nets(self):
        m = json.loads(MANIFEST.read_text())
        self.assertTrue(m["provisional"])
        netlist = json.loads((ROOT / "hardware/kicad/TL-DBG-001/intended-netlist.json").read_text())
        u = netlist["parts"][m["bridge_ref"]]
        by_name = {u["pin_names"][p]: net for p, net in u["pins"].items()}
        for gp, spec in m["gp"].items():
            self.assertEqual(by_name[gp], spec["net"], gp)

    def test_outputs_default_low(self):
        # Rails must stay off at power-up: every output's flash value is 0.
        man = Manifest.load()
        for role in ("aon_enable", "sw_enable"):
            s = man.required[man.pins[role]]
            self.assertFalse(s.input)
            self.assertFalse(s.value)


class ModuleTableMatchesFirmware(unittest.TestCase):
    def test_codes_and_manifest_ids(self):
        header = (ROOT / "firmware/portable/modules.h").read_text()
        source = (ROOT / "firmware/portable/modules.cc").read_text()
        enum = {name: int(val, 16) for name, val in re.findall(r"(k\w+)\s*=\s*(0x[0-9A-Fa-f]+)", header)}
        registry = dict(re.findall(r"\{ModuleCode::(k\w+),\s*\"([^\"]+)\"", source))
        firmware = {enum[name]: mid for name, mid in registry.items()}
        self.assertEqual(MODULES, firmware)

    def test_every_module_has_its_manifest(self):
        for code, mid in MODULES.items():
            path = ROOT / "contracts/manifests/modules" / f"{mid}.yaml"
            self.assertTrue(path.exists(), f"0x{code:04X}: {path} missing")
            ids = re.findall(r"^id:\s*(\S+)", path.read_text(), re.M)
            self.assertEqual(ids, [mid], path)

    def test_every_rev_a_card_has_a_code(self):
        for card, code in CARDS.items():
            self.assertIn(code, MODULES, card)
            self.assertTrue((ROOT / "hardware/kicad" / card).is_dir(), card)


class FirmwareBoardTableMatchesSchematic(unittest.TestCase):
    """firmware/esp_idf/main/board_tl_sn_001.h expander pins land on the right nets."""

    SLOT_NETS = {"sw_enable": "SLOT_SW_EN{}", "aon_enable": "SLOT_AON_EN{}",
                 "reset_n": "SLOT{}_RESET_N", "cs_n": "SLOT{}_CS_N",
                 "present_n": "SLOT{}_PRESENT_N", "alert_n": "SLOT{}_ALERT_N"}
    BOARD_NETS = {"uart_sel0": "UART_SEL0", "uart_sel1": "UART_SEL1",
                  "uart_mux_en_n": "UART_MUX_EN_N", "present_probe": "PRESENT_PROBE",
                  "charger_int_n": "CHG_INT_N", "charger_pg_n": "CHG_PG_N", "source_st": "PMUX_ST"}

    def setUp(self):
        self.header = (ROOT / "firmware/esp_idf/main/board_tl_sn_001.h").read_text()
        parts = json.loads((ROOT / "hardware/kicad/TL-SN-001/intended-netlist.json").read_text())["parts"]
        consts = {k: int(v, 16) for k, v in re.findall(r"(kExp[AB])\s*=\s*(0x[0-9A-Fa-f]+)", self.header)}
        # U15 (A0 = GND) is 0x20, U16 (A0 = V3V3) is 0x21.
        refs = {0x20: "U15", 0x21: "U16"}
        self.addr = {name: refs[a] for name, a in consts.items()}
        self.parts = parts

    def net(self, chip: str, pin: int) -> str:
        package_pin = 4 + pin if pin < 8 else 13 + (pin - 8)  # TCA9535 PW: P00-07 pins 4-11, P10-17 13-20
        return self.parts[self.addr[chip] if chip in self.addr else chip]["pins"][str(package_pin)]

    def test_slot_pins(self):
        for field, pattern in self.SLOT_NETS.items():
            m = re.search(rf"io\.{field}\s*=\s*\{{(kExp[AB]),\s*(?:static_cast<std::uint8_t>\()?(?:(\d+)\s*\+\s*)?i\)?\}}", self.header)
            self.assertIsNotNone(m, field)
            chip, base = m.group(1), int(m.group(2) or 0)
            for slot in range(4):
                self.assertEqual(self.net(self.addr[chip], base + slot), pattern.format(slot), f"{field} slot {slot}")

    def test_board_pins(self):
        for field, net in self.BOARD_NETS.items():
            m = re.search(rf"b\.{field}\s*=\s*\{{(kExp[AB]),\s*(\d+)\}}", self.header)
            self.assertIsNotNone(m, field)
            self.assertEqual(self.net(self.addr[m.group(1)], int(m.group(2))), net, field)

    def test_charger_address(self):
        self.assertRegex(self.header, r"kCharger\s*=\s*0x6A")


if __name__ == "__main__":
    unittest.main()
