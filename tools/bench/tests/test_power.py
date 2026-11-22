import unittest

from talos_bench import power


class Analyse(unittest.TestCase):
    def test_sleep_floor_ignores_bursts(self):
        m = power.MockMeter(sleep_ua=31.0, burst_ma=45.0, burst_ms=6.0, period_s=10.0, hz=10_000)
        r = power.analyse(m.read_ua(60.0), hz=10_000, source_mv=3_000)
        self.assertAlmostEqual(r.sleep_ua, 31.0, places=3)
        self.assertTrue(r.sleep_ok)
        self.assertEqual(r.bursts, 6)
        self.assertAlmostEqual(r.peak_ma, 45.0)
        self.assertAlmostEqual(r.burst_ms, 6.0, places=1)
        # Average includes the bursts: 31 uA + 45 mA * 6 ms / 10 s = 58 uA.
        self.assertAlmostEqual(r.average_ua, 31.0 + 45_000 * 0.006 / 10.0, delta=0.5)

    def test_high_floor_fails_hwr_009(self):
        m = power.MockMeter(sleep_ua=52.0, hz=10_000)
        r = power.analyse(m.read_ua(20.0), hz=10_000)
        self.assertFalse(r.sleep_ok)
        self.assertIn("FAIL", r.lines()[0])

    def test_regulator_input_flags_tired_cells_in_the_cold(self):
        r = power.PowerReport(sleep_ua=30, average_ua=40, peak_ma=450.0, burst_ms=5, bursts=1,
                              seconds=10, source_mv=2_200)
        self.assertGreaterEqual(r.regulator_input_mv(0.30), power.TPS63900_VIN_MIN_MV)
        self.assertLess(r.regulator_input_mv(1.00), power.TPS63900_VIN_MIN_MV)
        self.assertTrue(any("BELOW 1.8 V" in line for line in r.lines()))

    def test_no_quiet_window_is_infinite_not_a_pass(self):
        r = power.analyse([5_000.0] * 20_000, hz=10_000)
        self.assertFalse(r.sleep_ok)

    def test_measure_settles_then_analyses(self):
        m = power.MockMeter(hz=10_000, period_s=5.0)
        r = power.measure(m, seconds=20.0, source_mv=3_000, settle_s=1.0, hz=10_000)
        self.assertTrue(r.sleep_ok)
        self.assertEqual(r.source_mv, 3_000)

    def test_cli_mock(self):
        self.assertEqual(power.main(["--mock", "--seconds", "30"]), 0)


if __name__ == "__main__":
    unittest.main()
