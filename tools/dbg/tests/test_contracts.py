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
        self.consts = {k: int(v, 16) for k, v in re.findall(r"(kExp[ABC])\s*=\s*(0x[0-9A-Fa-f]+)", self.header)}
        # Each TCA9535 by its address straps (A0/A1/A2 = PW pins 21/2/3).
        self.parts = parts
        refs = {a: self.expander_at(a) for a in self.consts.values()}
        self.addr = {name: refs[a] for name, a in self.consts.items() if refs[a]}

    def expander_at(self, addr):
        for ref, part in self.parts.items():
            if part["value"].startswith("TCA9535"):
                p = part["pins"]
                bits = [p.get(pin) not in ("GND", None) for pin in ("21", "2", "3")]
                if 0x20 | bits[0] | (bits[1] << 1) | (bits[2] << 2) == addr:
                    return ref
        return None

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

    def test_expander_addresses(self):
        self.assertEqual(self.consts, {"kExpA": 0x20, "kExpB": 0x21, "kExpC": 0x22})
        self.assertEqual(self.addr.get("kExpA"), "U15")
        self.assertEqual(self.addr.get("kExpB"), "U16")

    def test_exp_c_pins(self):
        """Slot LEDs on EXP_C P00-P03, TAMPER_N on P04 (practices review L-03, HD-032)."""
        m = re.search(r"b\.slot_led_n\[i\]\s*=\s*\{kExpC,\s*i\}", self.header)
        self.assertIsNotNone(m, "slot_led_n")
        self.assertRegex(self.header, r"b\.tamper_n\s*=\s*\{kExpC,\s*4\}")
        self.assertRegex(self.header, r"b\.unused_pins\[0\]\s*=\s*\{kExpC,\s*0xFFE0\}")
        if "kExpC" not in self.addr:
            self.skipTest("netlist has no TCA9535 at 0x22 yet (generator change pending)")
        for slot in range(4):
            self.assertEqual(self.net("kExpC", slot), f"SLOT_LED{slot}_N")
        self.assertEqual(self.net("kExpC", 4), "TAMPER_N")


class BoardManifestMatchesFirmware(unittest.TestCase):
    """contracts/manifests/boards/sensor_node_proto_a.yaml documents what board_tl_sn_001.h drives."""

    def setUp(self):
        self.header = (ROOT / "firmware/esp_idf/main/board_tl_sn_001.h").read_text()
        self.manifest = (ROOT / "contracts/manifests/boards/sensor_node_proto_a.yaml").read_text()

    def const(self, name):
        return int(re.search(rf"{name}\s*=\s*(\w+);", self.header).group(1), 0)

    def test_provisional(self):
        self.assertRegex(self.manifest, r"(?m)^provisional: true$")

    def test_wake_lines_all_active_high_with_exp_wake_on_its_gpio(self):
        lines = re.findall(r"\{net: (\w+), mcu_pin: GPIO(\d+), active_high: (\w+)", self.manifest)
        self.assertEqual({net: int(g) for net, g, _ in lines},
                         {"SLOT_WAKE0": 10, "SLOT_WAKE1": 11, "SLOT_WAKE2": 12, "SLOT_WAKE3": 13,
                          "EXP_WAKE": self.const("kExpanderWake")})
        self.assertEqual({hi for _, _, hi in lines}, {"true"})  # one ext1 ANY_HIGH mask (HD-034)
        wake = re.search(r"kSlotWake\[kSlots\]\s*=\s*\{([^}]*)\}", self.header).group(1)
        self.assertEqual([int(x) for x in wake.split(",")], [10, 11, 12, 13])

    def test_status_led_and_exp_c_map(self):
        self.assertRegex(self.manifest, rf"\{{name: STAT, net: LED_K, driver: GPIO{self.const('kStatusLed')}, active_low: true")
        self.assertRegex(self.manifest, r"i2c_address: 34")
        for n in range(4):
            self.assertIn(f"{{pin: P0{n}, net: SLOT_LED{n}_N, direction: output, active_low: true}}", self.manifest)
        self.assertIn("{pin: P04, net: TAMPER_N, direction: input}", self.manifest)
        self.assertRegex(self.manifest, r"reserved_i2c_addresses: \[32, 33, 34, 106, 112\]")


if __name__ == "__main__":
    unittest.main()
