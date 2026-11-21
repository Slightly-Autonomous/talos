"""Unit tests for the hardware generators' pure pieces (run in CI's KiCad image; pcbnew-only tests skip without it).

    python3 -m unittest discover -s hardware/scripts/tests -t hardware/scripts -v
"""

from __future__ import annotations

import json
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
ROOT = HERE.parents[1]
KICAD = ROOT / "hardware" / "kicad"

try:
    import pcbnew  # noqa: F401
    HAVE_PCBNEW = True
except ImportError:
    HAVE_PCBNEW = False


def netlist(board: str) -> dict:
    return json.loads((KICAD / board / "intended-netlist.json").read_text())["parts"]


class SchematicIntent(unittest.TestCase):
    """What the owner-approved changes (2026-11-20) put in the netlists stays there."""

    def test_present_n_on_contact_19_everywhere(self):  # HD-025
        import gen_schematics as g
        self.assertEqual(g.EDGE[19], "PRESENT_N")
        self.assertEqual(g.EDGE[18], "ANALOG_RSVD")
        for card in ("TL-MOD-001", "TL-MOD-002", "TL-MOD-003", "TL-MOD-004", "TL-TEST-001"):
            pins = netlist(card)["J1"]["pins"]
            self.assertEqual(pins["19"], "GND", card)
            self.assertIsNone(pins["18"], card)

    def test_flipped_card_lands_present_on_host_gnd(self):  # HD-025: card contact c -> host 21 - c
        import gen_schematics as g
        self.assertEqual(g.EDGE[21 - 19], "GND")

    def test_tamper_rc_meets_the_bench_budget(self):  # B7: 1M x 10n, about 12 ms to 0.7 VCC
        sn = netlist("TL-SN-001")
        self.assertEqual(sn["R99"]["value"], "1M")
        self.assertEqual(sn["C38"]["value"], "10n")
        import math
        t_vih = -1e6 * 10e-9 * math.log(1 - 0.7)
        self.assertLess(t_vih + 0.050, 0.100)

    def test_exp_wake_is_the_inverted_interrupt(self):  # HD-034
        sn = netlist("TL-SN-001")
        self.assertEqual(sn["U1"]["pins"]["13"], "EXP_WAKE")
        self.assertEqual(sn["U19"]["pins"]["2"], "EXP_INT_N")
        self.assertEqual(sn["U19"]["pins"]["4"], "EXP_WAKE")
        for exp in ("U15", "U16", "U18"):
            self.assertEqual(sn[exp]["pins"]["1"], "EXP_INT_N", exp)

    def test_exp_c_address_and_pins(self):  # 0x22: A0 low, A1 high, A2 low
        u18 = netlist("TL-SN-001")["U18"]["pins"]
        self.assertEqual((u18["21"], u18["2"], u18["3"]), ("GND", "V3V3_CORE", "GND"))
        self.assertEqual([u18[p] for p in ("4", "5", "6", "7", "8")],
                         ["SLOT_LED0_N", "SLOT_LED1_N", "SLOT_LED2_N", "SLOT_LED3_N", "TAMPER_N"])

    def test_qod_to_vout_and_spi_pulldowns(self):  # HD-028, HD-027
        sn = netlist("TL-SN-001")
        for n in range(4):
            self.assertEqual(sn[f"U{6 + n}"]["pins"]["5"], f"V3V3_AON_{n}")
        pulled = {sn[r]["pins"]["1"] for r in ("R101", "R102", "R103") if sn[r]["pins"]["2"] == "GND"}
        self.assertEqual(pulled, {"SPI_SCK", "SPI_MOSI", "SPI_MISO"})

    def test_board_feature_footprints_stay_out_of_the_bom(self):
        import gen_schematics as g
        self.assertIn(g.FP_TAG, g.BOARD_FEATURES)
        self.assertNotIn(g.FP_HOOK, g.BOARD_FEATURES)  # the THT hooks are bought

    @unittest.skipUnless(HAVE_PCBNEW, "silk_pass imports pcbnew")
    def test_every_silk_label_names_a_real_part(self):
        import silk_pass
        for board, labels in silk_pass.LABELS.items():
            parts = netlist(board)
            for ref in labels:
                self.assertIn(ref, parts, f"{board} label for missing part {ref}")

    def test_every_sn_part_has_a_placement_group(self):
        import layout_groups
        g = layout_groups.build()
        self.assertEqual(g["_loose"], [])


class FabOutputs(unittest.TestCase):
    def test_ipc356_test_pad_map(self):
        import export_fab
        rec = "317" + "/V3V3_AON".ljust(14) + "   " + "TP1".ljust(6) + " -1    " + " " * 8 + "A01X+012345Y-006789X0800Y0800R000S0"
        other = "317" + "/GND".ljust(14) + "   " + "U1".ljust(6) + " -2    " + " " * 8 + "A02X+000100Y+000200"
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / "x.ipc356"
            p.write_text(rec + "\n" + other + "\n")
            rows = export_fab.test_pad_map(p).strip().splitlines()
        self.assertEqual(rows[0], "ref,net,x_mm,y_mm,side")
        self.assertEqual(rows[1:], ["TP1,V3V3_AON,12.345,-6.789,top"])

    def test_fab_notes_carry_the_order_options(self):
        import export_fab
        sn = export_fab.fab_notes("TL-SN-001")
        self.assertIn("POFV", sn)
        self.assertIn("JLCJLCJLCJLC", sn)
        card = export_fab.fab_notes("TL-MOD-001")
        self.assertIn("bevel", card)
        self.assertIn("0.65 mm", card)

    def test_pv_thermal_case(self):  # HD-041
        import pi_config
        pv = {(n, r): val for n, r, val, _ in pi_config.fields("pv")}
        self.assertEqual(pv[("PI_THERMAL", "U4")], "p=0.72W")
        self.assertIn(("PI_REG", "J2"), pv)
        self.assertNotIn(("PI_REG", "J1"), pv)
        usb = {(n, r): val for n, r, val, _ in pi_config.fields("usb")}
        self.assertEqual(usb[("PI_THERMAL", "U4")], "p=0.42W")


@unittest.skipUnless(HAVE_PCBNEW, "needs KiCad's pcbnew")
class PlacementMath(unittest.TestCase):
    def test_macro_pose_follows_the_anchor(self):
        import gen_pcbs
        self.assertEqual(gen_pcbs.macro_pose((10.0, 20.0, 0), (3.0, 0.0), 90), (13.0, 20.0, 90))
        x, y, r = gen_pcbs.macro_pose((10.0, 20.0, 180), (3.0, 1.0), 90)
        self.assertAlmostEqual(x, 7.0)
        self.assertAlmostEqual(y, 19.0)
        self.assertEqual(r, 270)

    def test_rot_is_a_rotation(self):
        import gen_pcbs
        for deg in (0, 90, 180, 270):
            x, y = gen_pcbs._rot(3.0, 4.0, deg)
            self.assertAlmostEqual(x * x + y * y, 25.0)
        self.assertEqual(gen_pcbs._rot(*gen_pcbs._rot(1.0, 2.0, 90), 270), (1.0, 2.0))

    def test_slot_twins_match_including_test_pads(self):
        import gen_pcbs
        parts = netlist("TL-SN-001")
        groups = gen_pcbs.load_groups("TL-SN-001")
        maps = gen_pcbs.slot_correspondence(parts, groups, 4, gen_pcbs.SPECS["TL-SN-001"]["pinned"])
        self.assertEqual(maps[1]["TP28"], "TP29")  # V3V3_AON_0 pad -> V3V3_AON_1 pad
        self.assertEqual(maps[3]["J4"] if "J4" in maps[3] else None, None)  # sockets are pinned, not copied


if __name__ == "__main__":
    unittest.main()
