"""The bench runner against the virtual board: a healthy node passes, and every injected
fault leads to the right diagnosis with its consequences marked blocked, not failed."""

import os
import unittest

from talos_bench.runner import Bench, plan, summary
from talos_hil.console import Console, ProcessTransport

BENCH_NODE = os.environ.get("TALOS_BENCH_NODE")
EXPECT = {0: "TL-TEST-001", 1: "TL-MOD-003", 2: "TL-MOD-004"}


def run(faults=(), expect=EXPECT, seated=None):
    seated = expect if seated is None else seated
    argv = [BENCH_NODE] + [f"--slot{s}={c}" for s, c in seated.items()] + [f"--fault={f}" for f in faults]
    transport = ProcessTransport(argv)
    try:
        results = Bench(Console(transport), expect).run(plan(expect))
    finally:
        transport.close()
    return {r.test: r for r in results}, results


@unittest.skipUnless(BENCH_NODE, "set TALOS_BENCH_NODE (bazel build //firmware/sim:bench_node)")
class Runner(unittest.TestCase):
    def assertDiagnosis(self, results, test, *fragments):
        r = results[test]
        self.assertEqual(r.verdict, "fail", f"{test}: {r.verdict} {r.detail}")
        for f in fragments:
            self.assertIn(f, r.diagnosis, f"{test}: {r.diagnosis}")

    def test_healthy_board_passes_everything(self):
        by_id, results = run()
        self.assertEqual({r.verdict for r in results}, {"pass"}, [(r.test, r.verdict, r.diagnosis) for r in results])
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
        self.assertDiagnosis(by_id, "slot3.discovered", "no card seen", "finger 18")

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
