"""A behavioural MCP2221A plus the TL-DBG-001 slot behind it, for tests.

Models what the tool depends on: the HID report layouts, the factory GP defaults (which power
the slot), flash vs run-time GP settings, the 1 MOhm rail pull-downs, and a card whose I2C
devices only answer while AON is on. It is written from the datasheet, so it shares the
datasheet reading with the driver; a bench run is still the real check.
"""

from __future__ import annotations

from collections import deque

# Factory GP designations as Table 3-6 encodes them on read:
# GP0 LED_URx (010), GP1 LED_UTx (011), GP2 USB/USBCFG (001), GP3 LED_I2C (001).
FACTORY_GP = [0x02, 0x03, 0x01, 0x01]


class IdEeprom:
    def __init__(self, code: int, revision: int):
        self.mem = bytearray(256)
        self.mem[0], self.mem[1], self.mem[2] = code >> 8, code & 0xFF, revision
        self.ptr = 0

    def write(self, data: bytes) -> None:
        self.ptr = data[0]

    def read(self, n: int) -> bytes:
        out = bytes(self.mem[(self.ptr + i) % 256] for i in range(n))
        self.ptr = (self.ptr + n) % 256
        return out


class FakeMcp2221:
    def __init__(self, serial: str = "01234567", flash_gp: list[int] | None = None):
        self.serial = serial
        self.flash_gp = list(flash_gp or FACTORY_GP)
        self.product = "MCP2221 USB-I2C/UART Combo"
        self.card: dict[int, IdEeprom] = {}
        self.wake = False
        self.alert_n = True
        self.flash_writes = 0
        self.power_up()

    # --- slot side ---------------------------------------------------------------------

    def power_up(self) -> None:
        """USB plug-in: run-time settings load from flash, outputs take power-up values."""
        self.sram_gp = list(self.flash_gp)
        self.out = [bool(b & 0x10) for b in self.sram_gp]
        self._i2c_state = 0
        self._nack = False
        self._read_buf: bytes | None = None
        self._pending: deque[bytes] = deque()

    def _gpio(self, n: int) -> bool:
        return (self.sram_gp[n] & 0x07) == 0

    def _input(self, n: int) -> bool:
        return bool(self.sram_gp[n] & 0x08)

    def _drive(self, n: int, factory_level: bool) -> bool:
        """Level on a rail enable: a GPIO output drives, an input floats to the 1M pull-down."""
        if not self._gpio(n):
            return factory_level
        return False if self._input(n) else self.out[n]

    @property
    def aon_on(self) -> bool:
        return self._drive(0, factory_level=True)  # LED_URx idles high

    @property
    def sw_on(self) -> bool:
        return self._drive(2, factory_level=True)  # USBCFG high once enumerated

    def _pin_level(self, n: int) -> bool:
        if n == 0:
            return self.aon_on
        if n == 2:
            return self.sw_on
        if n == 1:
            return self.wake
        return self.alert_n

    # --- HID ---------------------------------------------------------------------------

    def write(self, report: bytes) -> None:
        assert len(report) == 64, "every report is 64 bytes"
        r = bytearray(64)
        r[0] = report[0]
        handler = getattr(self, f"_cmd_{report[0]:02x}", None)
        if handler is None:
            raise AssertionError(f"command 0x{report[0]:02X} not modelled")
        if handler(report, r) is not False:
            self._pending.append(bytes(r))

    def read(self) -> bytes:
        return self._pending.popleft()

    def _cmd_10(self, c: bytes, r: bytearray) -> None:  # status / set parameters
        if c[2] == 0x10:
            r[2] = 0x10 if self._i2c_state else 0x11
            self._i2c_state, self._nack = 0, False
        if c[3] == 0x20:
            r[3], r[4] = 0x20, c[4]
        r[8] = self._i2c_state
        r[20] = 0x40 if self._nack else 0x00

    def _cmd_b0(self, c: bytes, r: bytearray) -> None:  # read flash
        if c[1] == 0x01:
            r[2] = 4
            r[4:8] = bytes(self.flash_gp)
        elif c[1] == 0x03:
            enc = self.product.encode("utf-16-le")
            r[2], r[3] = 2 + len(enc), 0x03
            r[4 : 4 + len(enc)] = enc
        elif c[1] == 0x05:
            r[2] = len(self.serial)
            r[4 : 4 + len(self.serial)] = self.serial.encode()
        else:
            r[1] = 0x01

    def _cmd_b1(self, c: bytes, r: bytearray) -> None:  # write flash
        self.flash_writes += 1
        if c[1] == 0x01:
            self.flash_gp = list(c[2:6])
        elif c[1] == 0x03:
            assert c[3] == 0x03
            self.product = bytes(c[4 : 4 + c[2] - 2]).decode("utf-16-le")
        else:
            r[1] = 0x02

    def _cmd_60(self, c: bytes, r: bytearray) -> None:  # set SRAM
        assert c[2:7] == bytes(5), "test only expects GP changes"
        if c[7] & 0x80:
            self.sram_gp = list(c[8:12])
            self.out = [bool(b & 0x10) for b in self.sram_gp]

    def _cmd_50(self, c: bytes, r: bytearray) -> None:  # set GPIO outputs
        for n in range(4):
            base = 2 + 4 * n
            if not self._gpio(n):
                r[base : base + 4] = b"\xEE" * 4
                continue
            r[base : base + 4] = c[base : base + 4]
            if c[base] and not self._input(n):
                self.out[n] = bool(c[base + 1])

    def _cmd_51(self, c: bytes, r: bytearray) -> None:  # get GPIO
        for n in range(4):
            if self._gpio(n):
                r[2 + 2 * n], r[3 + 2 * n] = int(self._pin_level(n)), int(self._input(n))
            else:
                r[2 + 2 * n], r[3 + 2 * n] = 0xEE, 0xEF

    def _device(self, addr8: int) -> IdEeprom | None:
        return self.card.get(addr8 >> 1) if self.aon_on else None

    def _i2c_write(self, c: bytes, r: bytearray) -> None:
        dev = self._device(c[3])
        length = c[1] | (c[2] << 8)
        if dev is None:
            self._nack = True
        else:
            dev.write(bytes(c[4 : 4 + length]))
            self._nack = False
        self._i2c_state = 0

    _cmd_90 = _i2c_write
    _cmd_94 = _i2c_write

    def _i2c_read(self, c: bytes, r: bytearray) -> None:
        dev = self._device(c[3])
        n = c[1] | (c[2] << 8)
        self._read_buf = dev.read(n) if dev else None

    _cmd_91 = _i2c_read
    _cmd_93 = _i2c_read

    def _cmd_40(self, c: bytes, r: bytearray) -> None:  # get I2C data
        if self._read_buf is None:
            r[1], r[3] = 0x00, 127
        else:
            r[3] = len(self._read_buf)
            r[4 : 4 + len(self._read_buf)] = self._read_buf
            self._read_buf = None

    def _cmd_70(self, c: bytes, r: bytearray) -> bool:  # reset: no response
        self.power_up()
        return False
