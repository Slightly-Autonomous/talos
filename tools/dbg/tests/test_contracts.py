"""The tool's tables must match the schematic and the firmware registry."""

import json
import pathlib
import re
import unittest

from talos_dbg.adapter import MANIFEST, Manifest
from talos_dbg.modules import MODULES

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


if __name__ == "__main__":
    unittest.main()
