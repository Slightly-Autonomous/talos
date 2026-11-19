"""A behavioural MCP2221A plus the TL-DBG-001 slot behind it, for tests.

Models what the tool depends on: the HID report layouts, the factory GP defaults (which power
the slot), flash vs run-time GP settings, the 1 MOhm rail pull-downs, the bus line levels
(pulled up to AON), AON's slow discharge (no quick-discharge on that switch), the GP1 edge
detector, and cards whose I2C devices only answer while AON is up. Written from the datasheets,
so it shares the datasheet reading with the driver; a bench run is still the real check.
"""

from __future__ import annotations

from collections import deque

# Factory GP designations as Table 3-6 encodes them on read:
# GP0 LED_URx (010), GP1 LED_UTx (011), GP2 USB/USBCFG (001), GP3 LED_I2C (001).
FACTORY_GP = [0x02, 0x03, 0x01, 0x01]
AON_DECAY_READS = 5  # status reads before a switched-off AON rail has fallen


def _crc8(data: bytes) -> int:
    crc = 0xFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


class Device:
    """An I2C target. write() returns False for a NACK; read() returns None for a NACK."""

    def write(self, data: bytes) -> bool:
        raise NotImplementedError

    def read(self, n: int) -> bytes | None:
        raise NotImplementedError

    def power_on_reset(self) -> None:
        pass


class IdEeprom(Device):
    """24AA025E48: 16-byte pages that wrap, upper half write-protected, busy after a write."""

    def __init__(self, code: int | None, revision: int = 1, eui: bytes = b"\x00\x04\xA3\x12\x34\x56",
                 legacy: bool = False):
        self.mem = bytearray(b"\xFF" * 256)
        if code is not None:
            if legacy:  # pre-v1 cards: code and revision only
                self.mem[0:3] = bytes([code >> 8, code & 0xFF, revision])
            else:
                body = bytes([code >> 8, code & 0xFF, revision, 0x01, 0]) + b"\xFF" * 10
                self.mem[0:16] = body + bytes([_crc8(body)])
        self.mem[0xFA:0x100] = eui
        self.ptr = 0
        self.busy = 0  # transactions to NACK after a write (TWC)
        self.writes = 0

    def write(self, data: bytes) -> bool:
        if self.busy:
            self.busy -= 1
            return False
        self.ptr = data[0]
        payload = data[1:]
        if payload:
            page = self.ptr & 0xF0
            for i, b in enumerate(payload):
                addr = page | ((self.ptr + i) & 0x0F)  # wraps inside the page
                if addr < 0x80:
                    self.mem[addr] = b
            self.writes += 1
            self.busy = 3
        return True

    def read(self, n: int) -> bytes | None:
        if self.busy:
            self.busy -= 1
            return None
        out = bytes(self.mem[(self.ptr + i) % 256] for i in range(n))
        self.ptr = (self.ptr + n) % 256
        return out


class Registers(Device):
    def __init__(self):
        self.regs = bytearray(256)
        self.ptr = 0

    def write(self, data: bytes) -> bool:
        self.ptr = data[0]
        for i, b in enumerate(data[1:]):
            self.regs[(self.ptr + i) % 256] = b
            self.on_write((self.ptr + i) % 256)
        return True

    def read(self, n: int) -> bytes | None:
        out = bytes(self.value((self.ptr + i) % 256) for i in range(n))
        self.ptr = (self.ptr + n) % 256
        return out

    def value(self, reg: int) -> int:
        return self.regs[reg]

    def on_write(self, reg: int) -> None:
        pass


class TestCardExpander(Registers):
    """TL-TEST-001 TCA9535 at 0x27: P00 drives WAKE, P01 drives ALERT_N (via 1k);
    P10 CS_N, P11 RESET_N (adapter pull-ups to AON), P14 UART_TX (U6, idle high) are inputs."""

    def __init__(self, bridge: "FakeMcp2221"):
        super().__init__()
        self.bridge = bridge
        self.power_on_reset()

    def power_on_reset(self) -> None:
        self.regs = bytearray(256)
        self.regs[0x02] = self.regs[0x03] = 0xFF
        self.regs[0x06] = self.regs[0x07] = 0xFF

    def _drives(self, pin: int) -> bool | None:
        port, bit = divmod(pin, 8)
        if self.regs[0x06 + port] >> bit & 1:
            return None  # input
        return bool(self.regs[0x02 + port] >> bit & 1)

    def on_write(self, reg: int) -> None:
        wake = self._drives(0)
        self.bridge.set_wake(bool(wake))  # released: the 1M pull-down holds WAKE low
        alert = self._drives(1)
        self.bridge.alert_n = alert is not False

    def value(self, reg: int) -> int:
        if reg == 0x01:
            aon = self.bridge.rail_up
            return (0x01 if aon else 0) | (0x02 if aon else 0) | (0x10 if aon else 0)
        if reg == 0x00:
            return (0x01 if self._drives(0) else 0) | (0x02 if self._drives(1) is not False else 0)
        return self.regs[reg]


class Tmag5273(Registers):
    """Enough of the TMAG5273A1 for the forced-threshold WAKE test."""

    def __init__(self, bridge: "FakeMcp2221", device_id: int = 0x01):
        super().__init__()
        self.bridge = bridge
        self.regs[0x0D] = device_id
        self.regs[0x0E], self.regs[0x0F] = 0x49, 0x54
        self.wake_path_open = False  # fault: inverter dead or MAG_INT_N open

    def write(self, data: bytes) -> bool:
        self.bridge.set_wake(False)  # addressing the device clears the latched INT
        ok = super().write(data)
        forced = (self.regs[0x01] & 0x03) == 0x03 and (self.regs[0x01] >> 5) == 0 and \
            self.regs[0x04] == 0x80 and self.regs[0x08] & 0x40
        if forced and not self.wake_path_open:
            self.bridge.set_wake(True)
        return ok

    def read(self, n: int) -> bytes | None:
        self.bridge.set_wake(False)
        return super().read(n)


class Lis2dw12(Registers):
    def __init__(self, self_test_mg: int = 400):
        super().__init__()
        self.regs[0x0F] = 0x44
        self.gravity = (0, 0, 1000)
        self.self_test_mg = self_test_mg

    def value(self, reg: int) -> int:
        if 0x28 <= reg <= 0x2D:
            axis, hi = divmod(reg - 0x28, 2)
            st = (self.regs[0x22] >> 6) & 3
            mg = self.gravity[axis] + (self.self_test_mg if st == 1 else -self.self_test_mg if st == 2 else 0)
            ug_per_lsb = 244 << ((self.regs[0x25] >> 4) & 3)
            raw = max(-8191, min(8191, mg * 1000 // ug_per_lsb)) * 4
            word = raw & 0xFFFF
            return word >> 8 if hi else word & 0xFF
        return self.regs[reg]


class Sht40(Device):
    def __init__(self, serial: int = 0x1234ABCD, temp_c: float = 22.0, heater_rise_c: float = 12.0):
        self.serial, self.temp_c, self.heater_rise_c = serial, temp_c, heater_rise_c
        self.out = bytes(6)

    def _load(self, a: int, b: int) -> None:
        wa, wb = a.to_bytes(2, "big"), b.to_bytes(2, "big")
        self.out = wa + bytes([_crc8(wa)]) + wb + bytes([_crc8(wb)])

    def _measure(self, t: float) -> None:
        self._load(round((t + 45) * 65535 / 175), round((45 + 6) * 65535 / 125))

    def write(self, data: bytes) -> bool:
        cmd = data[0]
        if cmd == 0x89:
            self._load(self.serial >> 16, self.serial & 0xFFFF)
        elif cmd == 0xFD:
            self._measure(self.temp_c)
        elif cmd == 0x2F:
            self._measure(self.temp_c + self.heater_rise_c)
        elif cmd != 0x94:
            return False
        return True

    def read(self, n: int) -> bytes | None:
        return self.out[:n]


class FakeMcp2221:
    def __init__(self, serial: str = "01234567", flash_gp: list[int] | None = None):
        self.serial = serial
        self.flash_gp = list(flash_gp or FACTORY_GP)
        self.product = "MCP2221 USB-I2C/UART Combo"
        self.card: dict[int, Device] = {}
        self.wake = False
        self.alert_n = True
        self.sda_held = False  # fault: something shorts SDA to GND
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
        self.irq_rising = self.irq_falling = False
        self.irq_flag = False
        self.rail_up = self.aon_on
        self._decay = 0

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

    def _rail_tick(self) -> None:
        """AON has no quick discharge: after the switch opens the rail takes a while to fall."""
        if self.aon_on:
            self.rail_up, self._decay = True, 0
        elif self.rail_up:
            self._decay += 1
            if self._decay >= AON_DECAY_READS:
                self.rail_up = False
                for dev in self.card.values():
                    dev.power_on_reset()

    def set_wake(self, level: bool) -> None:
        rose, fell = level and not self.wake, self.wake and not level
        if (rose and self.irq_rising) or (fell and self.irq_falling):
            self.irq_flag = True
        self.wake = level

    def _pin_level(self, n: int) -> bool:
        if n == 0:
            return self.aon_on
        if n == 2:
            return self.sw_on
        if n == 1:
            return self.wake
        return self.alert_n and self.rail_up  # pulled up to AON

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
        self._rail_tick()
        if c[2] == 0x10:
            r[2] = 0x10 if self._i2c_state else 0x11
            self._i2c_state, self._nack = 0, False
        if c[3] == 0x20:
            r[3], r[4] = 0x20, c[4]
        r[8] = self._i2c_state
        r[20] = 0x40 if self._nack else 0x00
        line = 1 if self.rail_up else 0
        r[22] = line
        r[23] = 0 if self.sda_held else line
        r[24] = 1 if self.irq_flag else 0

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
        assert c[2:6] == bytes(4), "test only expects GP and interrupt changes"
        b6 = c[6]
        if b6 & 0x80:
            if b6 & 0x10:
                self.irq_rising = bool(b6 & 0x08)
            if b6 & 0x04:
                self.irq_falling = bool(b6 & 0x02)
            if b6 & 0x01:
                self.irq_flag = False
        if c[7] & 0x80:
            self.sram_gp = list(c[8:12])
            self.out = [bool(b & 0x10) for b in self.sram_gp]
        self._rail_tick()

    def _cmd_50(self, c: bytes, r: bytearray) -> None:  # set GPIO outputs
        for n in range(4):
            base = 2 + 4 * n
            if not self._gpio(n):
                r[base : base + 4] = b"\xEE" * 4
                continue
            r[base : base + 4] = c[base : base + 4]
            if c[base] and not self._input(n):
                self.out[n] = bool(c[base + 1])
        self._rail_tick()

    def _cmd_51(self, c: bytes, r: bytearray) -> None:  # get GPIO
        for n in range(4):
            if self._gpio(n):
                r[2 + 2 * n], r[3 + 2 * n] = int(self._pin_level(n)), int(self._input(n))
            else:
                r[2 + 2 * n], r[3 + 2 * n] = 0xEE, 0xEF

    def _device(self, addr8: int) -> Device | None:
        if not self.rail_up or self.sda_held:
            return None
        return self.card.get(addr8 >> 1)

    def _i2c_write(self, c: bytes, r: bytearray) -> None:
        dev = self._device(c[3])
        length = c[1] | (c[2] << 8)
        self._nack = dev is None or not dev.write(bytes(c[4 : 4 + length]))
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
