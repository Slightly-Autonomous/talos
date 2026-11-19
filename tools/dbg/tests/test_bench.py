"""Stage D: card IDs, bus checks, AON power cycling, WAKE edges and the bench self-tests."""

import os
import pathlib
import tempfile
import unittest

from talos_dbg import id_block, selftest
from talos_dbg.adapter import AdapterError, BusHeldLow, DebugAdapter, card_addresses
from talos_dbg.mcp2221 import Mcp2221
from tests.fake_mcp2221 import (FakeMcp2221, IdEeprom, Lis2dw12, Sht40, TestCardExpander,
                                Tmag5273)


def bench(card=None):
    fake = FakeMcp2221()
    a = DebugAdapter(Mcp2221(fake, sleep=lambda s: None), sleep=lambda s: None)
    a.configure(no_card_seated=True)
    for addr, dev in (card or {}).items():
        fake.card[addr] = dev(fake) if callable(dev) and not isinstance(dev, IdEeprom) else dev
    return fake, a


def results(records):
    return {r["t"]: r["ok"] for r in records}


class IdBlockFormat(unittest.TestCase):
    def test_golden_vectors_match_the_firmware(self):
        # Same vectors as firmware/sim/node_test.cc (IdBlock.GoldenVectors).
        self.assertEqual(id_block.crc8(bytes([0xBE, 0xEF])), 0x92)
        self.assertEqual(id_block.encode(0x0302, 1).hex(), "0302010100ffffffffffffffffffffb0")

    def test_decode_cases(self):
        self.assertEqual(id_block.decode(id_block.encode(0x0103, 2)).status, "ok")
        self.assertEqual(id_block.decode(b"\xFF" * 16).status, "blank")
        bad = bytearray(id_block.encode(0x0103, 2))
        bad[15] ^= 1
        self.assertEqual(id_block.decode(bytes(bad)).status, "corrupt")
        legacy = bytes([0x01, 0x03, 0x01]) + b"\xFF" * 13
        self.assertEqual(id_block.decode(legacy).status, "corrupt")

    def test_eui_checks(self):
        self.assertEqual(id_block.eui_problems(b"\x00\x04\xA3\x12\x34\x56"), [])
        self.assertTrue(id_block.eui_problems(b"\xFF" * 6))
        self.assertTrue(id_block.eui_problems(b"\x01\x04\xA3\x12\x34\x56"))  # multicast


class ProgramId(unittest.TestCase):
    def setUp(self):
        self.log = pathlib.Path(tempfile.mkdtemp()) / "ids.jsonl"

    def test_blank_card_is_written_verified_and_survives_a_power_cycle(self):
        eeprom = IdEeprom(None)
        fake, a = bench({0x50: eeprom, 0x27: TestCardExpander})
        a.set_rail("aon", True)
        result = a.program_id("TL-TEST-001", log_path=self.log)
        self.assertEqual(result.status, "written")
        self.assertEqual(bytes(eeprom.mem[:16]), id_block.encode(0xF001, 1))
        self.assertEqual(eeprom.writes, 1)  # one page write
        self.assertIn("TL-TEST-001", self.log.read_text())
        self.assertEqual(a.program_id("TL-TEST-001", log_path=self.log).status, "already")
        self.assertEqual(eeprom.writes, 1)

    def test_refuses_to_overwrite_without_quoting_the_current_block(self):
        eeprom = IdEeprom(0x0302, 1)
        fake, a = bench({0x50: eeprom, 0x27: TestCardExpander})
        a.set_rail("aon", True)
        with self.assertRaisesRegex(AdapterError, "--expect-current"):
            a.program_id("TL-TEST-001", log_path=self.log)
        current = bytes(eeprom.mem[:16]).hex()
        self.assertEqual(a.program_id("TL-TEST-001", expect_current=current, log_path=self.log).status,
                         "written")

    def test_refuses_a_card_of_the_wrong_kind(self):
        fake, a = bench({0x50: IdEeprom(None), 0x44: Sht40()})  # an environment card
        a.set_rail("aon", True)
        with self.assertRaisesRegex(AdapterError, "refusing to write"):
            a.program_id("TL-TEST-001", log_path=self.log)

    def test_refuses_a_part_without_an_eui(self):
        fake, a = bench({0x50: IdEeprom(None, eui=b"\xFF" * 6), 0x27: TestCardExpander})
        a.set_rail("aon", True)
        with self.assertRaisesRegex(AdapterError, "blank"):
            a.program_id("TL-TEST-001", log_path=self.log)

    def test_card_addresses_come_from_the_manifests(self):
        self.assertEqual(card_addresses("TL-MOD-001"), {0x35, 0x50})
        self.assertEqual(card_addresses("TL-MOD-002"), {0x50})
        self.assertEqual(card_addresses("TL-TEST-001"), {0x27, 0x50})


class BusAndRails(unittest.TestCase):
    def test_sda_short_is_named_not_reported_as_nack(self):
        fake, a = bench({0x50: IdEeprom(0x0103)})
        a.set_rail("aon", True)
        fake.sda_held = True
        with self.assertRaisesRegex(BusHeldLow, "SDA held low"):
            a.scan()

    def test_aon_cycle_waits_for_the_rail_and_resets_the_card(self):
        fake, a = bench({0x50: IdEeprom(0x0103), 0x27: TestCardExpander})
        a.set_rail("aon", True)
        a.write(0x27, 0x06, b"\x00")
        took = a.power_cycle_aon()
        self.assertGreater(took, 0)  # a fixed 20 ms would not have reset it
        self.assertEqual(a.read(0x27, 0x06, 1), b"\xFF")

    def test_backfed_card_never_discharges(self):
        fake, a = bench()
        a.set_rail("aon", True)
        fake._rail_tick = lambda: None  # something holds AON up
        with self.assertRaisesRegex(AdapterError, "back-powering"):
            a.power_cycle_aon(timeout_s=0.05)

    def test_wake_latch_catches_a_pulse_and_keeps_the_rails(self):
        fake, a = bench()
        a.set_rail("aon", True)
        a.enter_wake_latch()
        self.assertTrue(fake.aon_on)
        self.assertFalse(a.take_wake_edge())
        fake.set_wake(True)
        fake.set_wake(False)  # shorter than any poll
        self.assertTrue(a.take_wake_edge())
        self.assertFalse(a.take_wake_edge())  # re-armed
        a.leave_wake_latch()
        self.assertTrue(fake.aon_on)
        self.assertFalse(a.lines().wake)


class SelfTests(unittest.TestCase):
    def test_adapter_stage_passes_with_an_empty_slot(self):
        fake, a = bench()
        r = results(selftest.adapter(a))
        self.assertTrue(all(v for v in r.values() if v is not None), r)
        self.assertIsNone(r["adapter.v3v3"])  # manual step, not a pass

    def test_adapter_stage_flags_a_seated_card(self):
        fake, a = bench({0x50: IdEeprom(0x0103)})
        self.assertFalse(results(selftest.adapter(a))["adapter.empty_scan"])

    def test_test_card_stage_passes(self):
        fake, a = bench({0x50: IdEeprom(0xF001), 0x27: TestCardExpander})
        r = results(selftest.test_card(a))
        failed = [k for k, v in r.items() if v is False]
        self.assertEqual(failed, [])

    def test_test_card_stage_names_a_broken_alert_contact(self):
        fake, a = bench({0x50: IdEeprom(0xF001), 0x27: TestCardExpander})
        card = fake.card[0x27]
        original = card.on_write

        def no_alert(reg):
            original(reg)
            fake.alert_n = True  # ALERT_N finger open: the pull never reaches GP3

        card.on_write = no_alert
        r = results(selftest.test_card(a))
        self.assertFalse(r["card.alert"])
        self.assertTrue(r["card.wake"])

    def test_tmag_wake_path(self):
        fake, a = bench({0x50: IdEeprom(0x0103), 0x35: Tmag5273})
        r = results(selftest.tmag(a))
        self.assertTrue(r["tmag.identity"] and r["tmag.wake_path"] and r["tmag.latch_clears"], r)
        fake.card[0x35].wake_path_open = True
        self.assertFalse(results(selftest.tmag(a))["tmag.wake_path"])

    def test_tmag_wrong_variant(self):
        fake, a = bench({0x50: IdEeprom(0x0103), 0x35: lambda f: Tmag5273(f, device_id=0x02)})
        self.assertFalse(results(selftest.tmag(a))["tmag.identity"])

    def test_imu_self_test(self):
        fake, a = bench({0x50: IdEeprom(0x0202), 0x19: Lis2dw12()})
        r = results(selftest.imu(a))
        self.assertTrue(r["imu.who_am_i"] and r["imu.gravity"] and r["imu.self_test"], r)
        fake.card[0x19] = Lis2dw12(self_test_mg=0)  # sensing element not responding
        self.assertFalse(results(selftest.imu(a))["imu.self_test"])

    def test_sht_heater(self):
        fake, a = bench({0x50: IdEeprom(0x0302), 0x44: Sht40()})
        r = results(selftest.sht(a))
        self.assertTrue(r["sht.serial"] and r["sht.heater"], r)
        fake.card[0x44] = Sht40(heater_rise_c=0)
        self.assertFalse(results(selftest.sht(a))["sht.heater"])


if __name__ == "__main__":
    unittest.main()
