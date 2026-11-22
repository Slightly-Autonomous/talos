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
    from boards import kicad_dir
    return json.loads((kicad_dir(board) / "intended-netlist.json").read_text())["parts"]


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


class SelfContainedNode(unittest.TestCase):
    """TL-SN-002 (ADR 0019): what the owner decided stays in the netlist."""

    def setUp(self):
        self.p = netlist("TL-SN-002")

    def test_five_active_high_wake_lines_on_lp_gpios(self):
        mcu = self.p["U1"]["pins"]  # pads 20, 21, 16, 12, 13 = GPIO10..14
        self.assertEqual([mcu[k] for k in ("20", "21", "16", "12", "13")],
                         ["MAG_WAKE", "ACC_INT", "PIR_OUT", "TAMPER", "USER_BTN"])
        self.assertEqual(self.p["U7"]["pins"]["4"], "MAG_WAKE")  # TMAG INT_N through the inverter
        self.assertEqual(self.p["SW3"]["pins"]["2"], "V3V3")  # USER pulls up: active high

    def test_usb_priority_mux_and_primary_cells_never_charged(self):
        u3 = self.p["U3"]["pins"]
        self.assertEqual((u3["3"], u3["5"], u3["6"]), ("USB_VBUS", "USB_VBUS", "VBAT"))  # priority mode
        self.assertFalse(any("BQ25" in q["value"] for q in self.p.values()))  # no charger at all
        self.assertEqual(self.p["Q1"]["pins"]["3"], "BATT_RAW")  # reverse-cell P-FET on the cells

    def test_every_sensor_and_the_pir_are_fitted(self):
        values = {q["value"] for q in self.p.values() if not q.get("dnp")}
        for v in ("TMAG5273A2QDBVT", "LIS2DW12TR", "SHT40-AD1B", "PIR223D-S0", "TCA9535PWR"):
            self.assertIn(v, values)

    def test_usb_c_buttons_and_ten_lamps(self):
        self.assertIn("J1", self.p)
        self.assertEqual({self.p[r]["value"] for r in ("SW1", "SW2", "SW3")}, {"RESET", "BOOT", "USER"})
        lamps = [r for r, q in self.p.items() if q["lib_id"] == "Device:LED"]
        self.assertEqual(len(lamps), 10)

    def test_lid_and_battery_door_switches_share_the_tamper_loop(self):  # HD-049
        """SW4 (lid) and SW5 (battery door) in series on TAMPER; TAMPER_DOOR on EXP P11, the pin the
        firmware table reads; J9 is an unfitted option behind the bridged JP3."""
        import re
        p = self.p
        self.assertEqual(set(p["SW4"]["pins"].values()), {"TAMPER", "TAMPER_LOOP"})
        self.assertEqual(set(p["SW5"]["pins"].values()), {"TAMPER_DOOR", "GND"})
        self.assertEqual(set(p["JP3"]["pins"].values()), {"TAMPER_LOOP", "TAMPER_DOOR"})
        self.assertTrue(p["J9"]["dnp"])
        self.assertEqual(p["U14"]["pins"]["14"], "TAMPER_DOOR")  # TCA9535 pin 14 = P11
        hdr = (ROOT / "firmware" / "esp_idf" / "main" / "board_tl_sn_002.h").read_text()
        self.assertRegex(hdr, r"kBatteryDoorPin = 9;")  # P11 = expander pin index 9
        self.assertIn("b.battery_door = {kExp, kBatteryDoorPin};", hdr)
        r31 = p["R31"]
        self.assertEqual(set(r31["pins"].values()), {"V3V3", "TAMPER_DOOR"})
        self.assertTrue(re.match(r"4\.7M", r31["value"]))

    def test_firmware_pin_table_matches_the_netlist(self):
        """board_tl_sn_002.h (firmware) and the generated netlist must name the same GPIOs (ADR 0013 rule:
        pins live in the BoardSupport table; this keeps it honest against the hardware)."""
        import csv
        import re
        hdr = (ROOT / "firmware" / "esp_idf" / "main" / "board_tl_sn_002.h").read_text()
        const = {m.group(1): int(m.group(2), 0) for m in
                 re.finditer(r"constexpr (?:std::uint8_t|int) (k\w+) =\s*(0x[0-9A-Fa-f]+|\d+);", hdr)}
        ledger = list(csv.DictReader(open(ROOT / "hardware" / "pins" / "esp32h2_mini1_pin_ledger.csv")))
        pad_of_gpio = {int(r["gpio"]): r["pad"] for r in ledger if r["gpio"].strip().isdigit()}
        mcu = self.p["U1"]["pins"]
        expect = {"kI2cSda": "I2C_SDA", "kI2cScl": "I2C_SCL", "kBatterySenseAdc": "VBAT_SENSE",
                  "kUsbPresent": "USB_PRESENT", "kStatusLed": "STATUS_LED_N", "kBootButton": "BOOT_N",
                  "kMagWake": "MAG_WAKE", "kAccInt": "ACC_INT", "kPirOut": "PIR_OUT", "kTamper": "TAMPER",
                  "kUserButton": "USER_BTN"}
        for name, net in expect.items():
            self.assertIn(name, const, f"{name} missing from board_tl_sn_002.h")
            self.assertEqual(mcu[pad_of_gpio[const[name]]], net, f"{name} = GPIO{const[name]}")
        exp = self.p["U14"]["pins"]  # TCA9535: P10 (pin 13) is expander pin 8 in firmware numbering
        self.assertEqual(const.get("kExp"), 0x20)
        self.assertEqual(exp["13"], "PIR_PWR")
        self.assertEqual(const.get("kPirPowerPin"), 8)

    def test_fully_jlcpcb_assembled(self):
        import csv
        rows = list(csv.DictReader(open(ROOT / "hardware" / "sourcing" / "jlc-parts.csv")))
        for ref, q in self.p.items():
            if q.get("dnp"):
                continue
            fp = (q["footprint"] or "").split(":")[-1]
            match = [r for r in rows if (r["value"] == q["value"] or r["value"] == "*") and r["footprint"] == fp]
            self.assertTrue(match, f"{ref} {q['value']} has no sourcing row")
            self.assertNotEqual(match[0]["assembly"], "not_placed", f"{ref} would need hand assembly")


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
