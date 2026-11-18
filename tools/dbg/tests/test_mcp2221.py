import unittest

from talos_dbg.mcp2221 import GpSetting, Mcp2221, Mcp2221Error, _utf16_descriptor
from tests.fake_mcp2221 import FakeMcp2221


class Encoding(unittest.TestCase):
    def test_gp_setting_round_trip(self):
        for b in range(0x20):
            self.assertEqual(GpSetting.decode(b).encode(), b)

    def test_gpio_output_low_is_zero(self):
        self.assertEqual(GpSetting(0, input=False, value=False).encode(), 0x00)
        self.assertEqual(GpSetting(0, input=True).encode(), 0x08)

    def test_string_descriptor(self):
        self.assertEqual(_utf16_descriptor("AB"), bytes([6, 3, 0x41, 0, 0x42, 0]))
        with self.assertRaises(ValueError):
            _utf16_descriptor("x" * 31)


class Commands(unittest.TestCase):
    def setUp(self):
        self.fake = FakeMcp2221(serial="ABC123")
        self.m = Mcp2221(self.fake, sleep=lambda s: None)

    def test_i2c_divider(self):
        # Table 3-1 note: divider = 12 MHz / rate - 2.
        self.m.set_i2c_speed(100_000)
        self.assertEqual(self.m.status()[0], 0x10)
        with self.assertRaises(ValueError):
            self.m.set_i2c_speed(10_000)

    def test_factory_serial_and_product(self):
        self.assertEqual(self.m.read_factory_serial(), "ABC123")
        self.m.write_product("TL-DBG-001")
        self.assertEqual(self.m.read_product(), "TL-DBG-001")

    def test_gpio_on_a_non_gpio_pin_is_an_error(self):
        with self.assertRaises(Mcp2221Error):
            self.m.set_gpio(0, False)
        self.assertEqual(self.m.get_gpio(), [None] * 4)

    def test_reserved_addresses_rejected(self):
        for addr in (0x00, 0x07, 0x78):
            with self.assertRaises(ValueError):
                self.m.i2c_read(addr, 1)

    def test_bad_echo_raises(self):
        self.fake.read = lambda: bytes(64)
        with self.assertRaises(Mcp2221Error):
            self.m.status()


if __name__ == "__main__":
    unittest.main()
