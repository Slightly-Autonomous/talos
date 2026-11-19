import unittest

from talos_dbg.adapter import AdapterError, DebugAdapter, NotConfigured
from talos_dbg.mcp2221 import Mcp2221
from tests.fake_mcp2221 import FakeMcp2221, IdEeprom

PIR = 0x0101


def make(card=None):
    fake = FakeMcp2221()
    if card is not None:
        fake.card.update(card)
    return fake, DebugAdapter(Mcp2221(fake, sleep=lambda s: None), sleep=lambda s: None)


def configured(card=None):
    fake, a = make(card)
    a.configure(no_card_seated=True)
    return fake, a


class FactoryFresh(unittest.TestCase):
    def test_factory_defaults_power_the_slot(self):
        # HW-0007: GP0 LED_URx idles high, GP2 USBCFG goes high once enumerated.
        fake, a = make()
        self.assertTrue(fake.aon_on)
        self.assertTrue(fake.sw_on)
        self.assertFalse(a.is_configured())

    def test_card_commands_refuse_an_unconfigured_adapter(self):
        fake, a = make({0x50: IdEeprom(PIR, 1)})
        for call in (a.lines, a.card_id, a.scan, a.all_off, lambda: a.set_rail("aon", False)):
            with self.assertRaises(NotConfigured):
                call()

    def test_configure_requires_the_no_card_confirmation(self):
        fake, a = make()
        with self.assertRaises(AdapterError):
            a.configure(no_card_seated=False)
        self.assertEqual(fake.flash_writes, 0)


class Configure(unittest.TestCase):
    def test_rails_go_off_and_stay_off_after_replug(self):
        fake, a = configured()
        self.assertFalse(fake.aon_on)
        self.assertFalse(fake.sw_on)
        self.assertTrue(a.is_configured())
        self.assertEqual(fake.product, "TL-DBG-001")
        fake.power_up()  # unplug and replug: settings come from flash
        self.assertFalse(fake.aon_on)
        self.assertFalse(fake.sw_on)
        self.assertTrue(a.is_configured())

    def test_configure_twice_does_not_rewrite_flash(self):
        fake, a = configured()
        writes = fake.flash_writes
        steps = a.configure(no_card_seated=True)
        self.assertEqual(fake.flash_writes, writes)
        self.assertIn("flash GP settings already correct", steps)

    def test_flash_that_will_not_take_the_settings_is_reported(self):
        fake, a = make()
        fake._cmd_b1 = lambda c, r: None  # accepts the write, keeps the old data
        with self.assertRaises(AdapterError):
            a.configure(no_card_seated=True)

    def test_wrong_power_up_value_counts_as_unconfigured(self):
        fake, a = configured()
        fake.flash_gp[0] |= 0x10  # AON enable would power up high
        self.assertFalse(a.is_configured())


class Rails(unittest.TestCase):
    def test_sequencing(self):
        fake, a = configured()
        with self.assertRaises(AdapterError):
            a.set_rail("sw", True)
        a.set_rail("aon", True)
        a.set_rail("sw", True)
        self.assertTrue(fake.aon_on and fake.sw_on)
        with self.assertRaises(AdapterError):
            a.set_rail("aon", False)
        a.all_off()
        self.assertFalse(fake.aon_on or fake.sw_on)

    def test_lines(self):
        fake, a = configured()
        fake.wake, fake.alert_n = True, False
        ln = a.lines()
        self.assertTrue(ln.wake)
        self.assertIsNone(ln.alert_asserted)  # ALERT_N is pulled up to AON: unknown while it is off
        self.assertFalse(ln.aon_on)
        a.set_rail("aon", True)
        self.assertTrue(a.lines().alert_asserted)


class CardIdentity(unittest.TestCase):
    def test_reads_the_id_eeprom(self):
        fake, a = configured({0x50: IdEeprom(PIR, 3)})
        a.set_rail("aon", True)
        cid = a.card_id()
        self.assertEqual((cid.code, cid.revision, cid.manifest_id), (PIR, 3, "pir_ekmb_v1"))

    def test_unpowered_card_is_an_error_not_an_empty_slot(self):
        fake, a = configured({0x50: IdEeprom(PIR, 1)})
        with self.assertRaises(AdapterError):
            a.card_id()

    def test_empty_slot(self):
        fake, a = configured()
        a.set_rail("aon", True)
        self.assertIsNone(a.card_id())

    def test_unknown_code_is_flagged(self):
        fake, a = configured({0x50: IdEeprom(0x7777, 1)})
        a.set_rail("aon", True)
        cid = a.card_id()
        self.assertIsNone(cid.manifest_id)
        self.assertIn("UNKNOWN", cid.describe())

    def test_scan(self):
        fake, a = configured({0x50: IdEeprom(PIR, 1), 0x19: IdEeprom(None)})
        a.set_rail("aon", True)
        self.assertEqual(a.scan(), [0x19, 0x50])


if __name__ == "__main__":
    unittest.main()
