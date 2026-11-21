"""The bench runner against the virtual board: a healthy node passes, and every injected
fault leads to the right diagnosis with its consequences marked blocked, not failed."""

import os
import unittest

from talos_bench.runner import Bench, judge_sw_current, plan, summary
from talos_hil.console import Console, ProcessTransport

BENCH_NODE = os.environ.get("TALOS_BENCH_NODE")
EXPECT = {0: "TL-TEST-001", 1: "TL-MOD-003", 2: "TL-MOD-004"}


def run(faults=(), expect=EXPECT, seated=None, extra=(), flipped=None, measure=None):
    seated = expect if seated is None else seated
    # A flipped card is physically there only when its "flipped<slot>" fault is injected.
    upside_down = {s: c for s, c in (flipped or {}).items() if f"flipped{s}" in faults}
    argv = ([BENCH_NODE] + [f"--slot{s}={c}" for s, c in {**seated, **upside_down}.items()]
            + [f"--fault={f}" for f in faults] + list(extra))
    transport = ProcessTransport(argv)
    try:
        results = Bench(Console(transport), expect, flipped=flipped, measure=measure).run(plan(expect, flipped))
    finally:
        transport.close()
    return {r.test: r for r in results}, results


class SwLampCurrent(unittest.TestCase):
    """T-11 fault tree, no node needed."""

    def test_window_and_diagnoses(self):
        self.assertEqual(judge_sw_current(10.2)[0], "pass")
        self.assertEqual(judge_sw_current(11.8)[0], "pass")
        self.assertIn("330 R load is open", judge_sw_current(0.5)[2])
        self.assertIn("lamp is missing, reversed", judge_sw_current(10.0)[2])
        self.assertIn("too small", judge_sw_current(13.0)[2])
        self.assertIn("short", judge_sw_current(40.0)[2])


@unittest.skipUnless(BENCH_NODE, "set TALOS_BENCH_NODE (bazel build //firmware/sim:bench_node)")
class Runner(unittest.TestCase):
    def assertDiagnosis(self, results, test, *fragments):
        r = results[test]
        self.assertEqual(r.verdict, "fail", f"{test}: {r.verdict} {r.detail}")
        for f in fragments:
            self.assertIn(f, r.diagnosis, f"{test}: {r.diagnosis}")

    def test_healthy_board_passes_everything(self):
        by_id, results = run()
        # Everything automatable passes; what needs eyes or a meter is "manual", never "pass".
        self.assertEqual({r.verdict for r in results}, {"pass", "manual"},
                         [(r.test, r.verdict, r.diagnosis) for r in results])
        self.assertEqual({r.test for r in results if r.verdict == "manual"}, {"C0-01", "D-10", "T-11"})
        self.assertEqual(summary(results)["failed"], [])

    def test_dead_branch(self):
        by_id, _ = run(["branch_open1"])
        self.assertDiagnosis(by_id, "slot1.discovered", "nothing answers on branch 1", "AON_1")
        self.assertEqual(by_id["slot1.imu_selftest"].verdict, "blocked")
        self.assertEqual(by_id["slot0.discovered"].verdict, "pass")  # other slots unaffected

    def test_card_whose_id_eeprom_is_dead(self):
        by_id, _ = run(["no_id2"])
        self.assertDiagnosis(by_id, "slot2.discovered", "sensors answer", "0x44", "card fault")

    def test_blank_card_id(self):
        by_id, _ = run(["blank_id0"])
        self.assertDiagnosis(by_id, "slot0.discovered", "never programmed", "program-id --card TL-TEST-001")

    def test_missing_card(self):
        by_id, _ = run(expect={**EXPECT, 3: "TL-MOD-004"}, seated=EXPECT)
        self.assertDiagnosis(by_id, "slot3.discovered", "no card seen", "finger 19")

    def test_open_wake_contact(self):
        by_id, _ = run(["card_wake_open0"])
        self.assertDiagnosis(by_id, "slot0.slot_selftest", "WAKE finger 9")
        self.assertNotIn("ALERT", by_id["slot0.slot_selftest"].diagnosis)

    def test_charger_not_answering(self):
        by_id, _ = run(["charger_nack"])
        self.assertDiagnosis(by_id, "S-M05", "charger U4 BQ25186")
        self.assertEqual(by_id["slot1.discovered"].verdict, "blocked")

    def test_expander_not_answering(self):
        by_id, _ = run(["expa_nack"])
        self.assertDiagnosis(by_id, "S-M00", "safe state")
        self.assertDiagnosis(by_id, "S-M05", "EXP_A U15")

    def test_board_identity_never_programmed(self):
        by_id, _ = run(extra=["--no-board-id"])
        self.assertDiagnosis(by_id, "S-M16", "never programmed", "program-id --node", "TL-SN-001")
        self.assertEqual(by_id["S-M05"].verdict, "pass")  # nothing else depends on it

    def test_board_identity_of_another_board(self):
        by_id, _ = run(extra=["--board-id=TL-SN-001:B:SN-7"])
        self.assertDiagnosis(by_id, "S-M16", "rev B", "expected TL-SN-001 rev A")

    def test_tamper_loop_open(self):
        by_id, _ = run(["lid_open"])
        self.assertDiagnosis(by_id, "S-M17", "TAMPER_N high", "J9")
        self.assertEqual(by_id["slot0.discovered"].verdict, "pass")

    def test_third_expander_not_answering(self):
        by_id, _ = run(["expc_nack"])
        self.assertDiagnosis(by_id, "S-M05", "EXP_C U18 (0x22")
        self.assertEqual(by_id["S-M17"].verdict, "blocked")

    def test_lamps_and_exp_c_unused_pins(self):
        by_id, _ = run()
        self.assertEqual(by_id["S-M18"].verdict, "pass", by_id["S-M18"].detail)
        self.assertIn("visible", by_id["S-M18"].detail["manual"])
        by_id, _ = run(["expc_bridge"])
        self.assertDiagnosis(by_id, "S-M18", "P12 read high", "solder bridge", "U18")
        by_id, _ = run(["no_vin"])  # on battery: lamps must be dark, service mode still lights them
        self.assertEqual(by_id["S-M18"].verdict, "pass", by_id["S-M18"].detail)
        self.assertEqual(by_id["S-M18"].detail["lit"], [0, 0, 0, 0])

    def test_tamper_during_deep_sleep(self):
        by_id, _ = run()
        self.assertEqual(by_id["S-M19"].verdict, "pass", by_id["S-M19"].detail)
        self.assertEqual(by_id["S-M19"].detail["tamper"], 1)
        by_id, _ = run(["exp_wake_dead"])
        self.assertDiagnosis(by_id, "S-M19", "did not wake on EXP_WAKE", "U19 SN74AUP1G04")
        by_id, _ = run(["expc_nack"])
        self.assertEqual(by_id["S-M19"].verdict, "blocked")

    def test_flipped_card(self):
        by_id, _ = run(["flipped3"], flipped={3: "TL-TEST-001"})
        self.assertEqual(by_id["C0-01.slot3"].verdict, "pass", by_id["C0-01.slot3"].detail)
        by_id, _ = run(flipped={3: "TL-TEST-001"}, seated=EXPECT)  # declared but not inserted
        self.assertNotIn(3, EXPECT)
        self.assertDiagnosis(by_id, "C0-01.slot3", "PRESENT reads empty", "HD-025")

    def test_sw_lamp_current(self):
        by_id, _ = run(measure={"T-11": 11.0})
        self.assertEqual(by_id["T-11"].verdict, "pass")
        by_id, _ = run(measure={"T-11": 10.0})
        self.assertDiagnosis(by_id, "T-11", "SW lamp is missing")
        self.assertIn("manual", {run()[0]["D-10"].verdict})

    def test_mux_not_answering(self):
        by_id, _ = run(["mux_nack"])
        self.assertDiagnosis(by_id, "S-M05", "I2C mux U14")

    def test_imu_sensing_element(self):
        by_id, _ = run(["imu_no_selftest1"])
        self.assertDiagnosis(by_id, "slot1.imu_selftest", "self-test response out of range")

    def test_sht_heater(self):
        by_id, _ = run(["sht_dead_heater2"])
        self.assertDiagnosis(by_id, "slot2.sht_selftest", "heater did not warm")

    def test_sht_missing_behind_a_good_id(self):
        by_id, _ = run(["sht_missing2"])
        self.assertDiagnosis(by_id, "slot2.discovered", "sensor did not start", "TL-DBG-001")


if __name__ == "__main__":
    unittest.main()
