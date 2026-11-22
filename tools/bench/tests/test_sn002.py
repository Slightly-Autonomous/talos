"""The TL-SN-002 bench plan (ADR 0019) against the virtual board: a healthy node passes, and
each injected fault leads to the right diagnosis with its consequences blocked, not failed."""

import os
import unittest

from talos_bench import sn002
from talos_bench.runner import Bench, summary
from talos_hil.console import Console, ProcessTransport

BENCH_NODE = os.environ.get("TALOS_BENCH_NODE")


def run(faults=(), extra=()):
    argv = [BENCH_NODE, "--board=TL-SN-002"] + [f"--fault={f}" for f in faults] + list(extra)
    transport = ProcessTransport(argv)
    try:
        results = Bench(Console(transport), {}).run(sn002.plan())
    finally:
        transport.close()
    return {r.test: r for r in results}, results


@unittest.skipUnless(BENCH_NODE, "set TALOS_BENCH_NODE (bazel build //firmware/sim:bench_node)")
class Sn002Bench(unittest.TestCase):
    def assertDiagnosis(self, results, test, *fragments):
        r = results[test]
        self.assertEqual(r.verdict, "fail", f"{test}: {r.verdict} {r.detail}")
        for f in fragments:
            self.assertIn(f, r.diagnosis, f"{test}: {r.diagnosis}")

    def test_healthy_board_passes_everything(self):
        by_id, results = run()
        self.assertEqual({r.verdict for r in results}, {"pass"},
                         [(r.test, r.verdict, r.diagnosis) for r in results])
        self.assertEqual(by_id["S-M16"].detail["board"], "TL-SN-002")
        self.assertEqual(by_id["S-M16"].detail["rev"], "A")
        for t in ("N-01.mag", "N-02.vib", "N-03.pir", "N-04.env", "N-07", "N-08", "N-09", "N-10",
                  "S-M20", "S-M21"):
            self.assertIn(t, by_id)
        # Lid closed: read-only console, refusal counted, every lamp dark even on USB.
        self.assertTrue(by_id["S-M20"].detail["console_locked"])
        self.assertEqual(by_id["S-M20"].detail["refusal"], "locked: open the enclosure")
        self.assertTrue(by_id["S-M20"].detail["external_power"])
        self.assertEqual(by_id["S-M20"].detail["lamps"], [0, 0, 0, 0])
        # Then the plan opened it for the rest.
        self.assertEqual(by_id["S-M21"].detail, {"tamper": 1, "console_locked": False})
        self.assertEqual(summary(results)["failed"], [])

    def test_missing_tmag(self):
        by_id, _ = run(["tmag_missing"])
        self.assertDiagnosis(by_id, "N-05", "TMAG5273A2 (0x35)")
        self.assertDiagnosis(by_id, "N-01.mag", "TMAG5273A2", "no card to reseat")
        self.assertEqual(by_id["N-02.vib"].verdict, "pass")  # the others keep working

    def test_dead_imu_self_test(self):
        by_id, _ = run(["imu_no_selftest"])
        self.assertDiagnosis(by_id, "N-06.imu", "LIS2DW12 self-test")

    def test_missing_sht_blocks_its_self_test(self):
        by_id, _ = run(["sht_missing"])
        self.assertDiagnosis(by_id, "N-04.env", "SHT40")
        self.assertEqual(by_id["N-06.sht"].verdict, "blocked")

    def test_lid_open(self):
        by_id, _ = run(["lid_open"])
        self.assertDiagnosis(by_id, "S-M17", "tamper loop open")
        for t in ("S-M19", "S-M20", "S-M21", "N-06.imu", "N-07", "N-08"):
            self.assertEqual(by_id[t].verdict, "blocked", t)

    def test_low_cells(self):
        by_id, _ = run(["battery_mv=2100"])
        self.assertDiagnosis(by_id, "N-12", "2.2 V under load")

    def test_wrong_board_identity(self):
        by_id, _ = run(extra=["--board-id=TL-SN-001:A:SIM-0001"])
        self.assertDiagnosis(by_id, "S-M16", "expected TL-SN-002 rev A")

    def test_unprogrammed_identity_points_at_the_flasher(self):
        by_id, _ = run(extra=["--no-board-id"])
        self.assertDiagnosis(by_id, "S-M16", "never programmed", "talos_bench.flash")
