"""MCP2221A USB-HID protocol (Microchip DS20005565E, section 3.1).

Every exchange is one 64-byte report out and one 64-byte report back. Only the commands the
TL-DBG-001 tool needs are here. The byte layouts follow the datasheet tables cited at each
method. Nothing has been run against a real chip yet: the I2C completion and NACK handling
in particular need bench verification (HW-0007).
"""

from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Protocol

REPORT = 64

CMD_STATUS = 0x10
CMD_READ_FLASH = 0xB0
CMD_WRITE_FLASH = 0xB1
CMD_I2C_WRITE = 0x90
CMD_I2C_WRITE_NO_STOP = 0x94
CMD_I2C_READ = 0x91
CMD_I2C_READ_RESTART = 0x93
CMD_I2C_GET_DATA = 0x40
CMD_SET_GPIO = 0x50
CMD_GET_GPIO = 0x51
CMD_SET_SRAM = 0x60
CMD_RESET = 0x70

FLASH_GP = 0x01
FLASH_PRODUCT = 0x03
FLASH_FACTORY_SERIAL = 0x05

DESIGNATION_GPIO = 0b000
DESIGNATION_GP1_INTERRUPT = 0b100  # GP1 alternate function 2
NOT_GPIO = 0xEE
I2C_CHUNK = 60
POLL_LIMIT = 50


class Mcp2221Error(Exception):
    pass


class I2cNack(Mcp2221Error):
    """No device acknowledged, or the transfer failed on the bus."""


class Hid(Protocol):
    def write(self, report: bytes) -> None: ...
    def read(self) -> bytes: ...


@dataclass(frozen=True)
class GpSetting:
    """One GP pin's settings byte (Tables 3-6, 3-13 and 3-36 share this layout)."""

    designation: int
    input: bool
    value: bool = False

    def encode(self) -> int:
        return (int(self.value) << 4) | (int(self.input) << 3) | (self.designation & 0x07)

    @staticmethod
    def decode(b: int) -> "GpSetting":
        return GpSetting(designation=b & 0x07, input=bool(b & 0x08), value=bool(b & 0x10))

    def is_gpio(self) -> bool:
        return self.designation == DESIGNATION_GPIO


def _utf16_descriptor(text: str) -> bytes:
    if len(text) > 30:
        raise ValueError("USB string descriptors hold at most 30 characters")
    return bytes([2 + 2 * len(text), 0x03]) + text.encode("utf-16-le")


class Mcp2221:
    def __init__(self, hid: Hid, sleep=time.sleep):
        self._hid = hid
        self._sleep = sleep

    def _xfer(self, *head: int, payload: bytes = b"") -> bytes:
        report = bytes(head) + payload
        if len(report) > REPORT:
            raise ValueError("report longer than 64 bytes")
        self._hid.write(report.ljust(REPORT, b"\x00"))
        resp = self._hid.read()
        if len(resp) < REPORT or resp[0] != head[0]:
            raise Mcp2221Error(f"bad response to 0x{head[0]:02X}: {resp[:4].hex()}")
        return resp

    # --- Status (Table 3-1, 3-2) -------------------------------------------------------

    def status(self) -> bytes:
        return self._xfer(CMD_STATUS, 0x00, 0x00, 0x00, 0x00)

    def set_i2c_speed(self, hz: int) -> None:
        divider = 12_000_000 // hz - 2
        if not 0 < divider < 256:
            raise ValueError(f"I2C speed {hz} Hz out of range")
        resp = self._xfer(CMD_STATUS, 0x00, 0x00, 0x20, divider)
        if resp[3] != 0x20:
            raise Mcp2221Error("I2C speed not accepted (transfer in progress?)")

    def cancel_i2c(self) -> None:
        self._xfer(CMD_STATUS, 0x00, 0x10)

    def line_levels(self) -> tuple[bool, bool]:
        """(SCL, SDA) as read from the pins (Table 3-2 bytes 22/23)."""
        st = self.status()
        return bool(st[22]), bool(st[23])

    def interrupt_flag(self) -> bool:
        """GP1 interrupt-on-change detector state (Table 3-2 byte 24)."""
        return bool(self.status()[24])

    # --- Flash (Tables 3-3 to 3-16) ----------------------------------------------------

    def _read_flash(self, sub: int) -> bytes:
        resp = self._xfer(CMD_READ_FLASH, sub)
        if resp[1] != 0x00:
            raise Mcp2221Error(f"read flash 0x{sub:02X} refused: 0x{resp[1]:02X}")
        return resp

    def _write_flash(self, sub: int, payload: bytes) -> None:
        resp = self._xfer(CMD_WRITE_FLASH, sub, payload=payload)
        if resp[1] != 0x00:
            raise Mcp2221Error(f"write flash 0x{sub:02X} refused: 0x{resp[1]:02X} (flash locked?)")

    def read_gp_flash(self) -> list[GpSetting]:
        resp = self._read_flash(FLASH_GP)
        return [GpSetting.decode(b) for b in resp[4:8]]

    def write_gp_flash(self, gp: list[GpSetting]) -> None:
        self._write_flash(FLASH_GP, bytes(s.encode() for s in gp))

    def read_product(self) -> str:
        resp = self._read_flash(FLASH_PRODUCT)
        n = max(0, min(resp[2], 62) - 2)
        return resp[4 : 4 + n].decode("utf-16-le", errors="replace")

    def write_product(self, text: str) -> None:
        self._write_flash(FLASH_PRODUCT, _utf16_descriptor(text))

    def read_factory_serial(self) -> str:
        resp = self._read_flash(FLASH_FACTORY_SERIAL)
        n = min(resp[2], 60)
        return resp[4 : 4 + n].decode("ascii", errors="replace").strip("\x00 ")

    # --- Run-time GP settings (Table 3-36) ---------------------------------------------

    def set_gp_sram(self, gp: list[GpSetting]) -> None:
        # Bytes 2-6 leave clock, DAC, ADC and interrupt settings alone (bit 7 clear).
        # Byte 7 bit 7 says "load the GP designations that follow".
        self._xfer(CMD_SET_SRAM, 0x00, 0, 0, 0, 0, 0, 0x80, *(s.encode() for s in gp))

    def set_interrupt(self, rising: bool, falling: bool, clear: bool = True) -> None:
        """Configure the GP1 edge detector (Table 3-36 byte 6) without touching GP settings."""
        b6 = 0x80 | 0x10 | (0x08 if rising else 0) | 0x04 | (0x02 if falling else 0) | (0x01 if clear else 0)
        self._xfer(CMD_SET_SRAM, 0x00, 0, 0, 0, 0, b6, 0x00)

    # --- GPIO (Tables 3-32 to 3-35) ----------------------------------------------------

    def set_gpio(self, pin: int, value: bool) -> None:
        if not 0 <= pin <= 3:
            raise ValueError("GP pin must be 0-3")
        body = [0] * 16
        body[pin * 4] = 0x01
        body[pin * 4 + 1] = 0x01 if value else 0x00
        resp = self._xfer(CMD_SET_GPIO, 0x00, *body)
        if resp[2 + pin * 4] == NOT_GPIO:
            raise Mcp2221Error(f"GP{pin} is not set for GPIO operation")

    def get_gpio(self) -> list[tuple[bool, bool] | None]:
        """Per pin (value, is_input), or None when the pin is not a GPIO."""
        resp = self._xfer(CMD_GET_GPIO)
        out: list[tuple[bool, bool] | None] = []
        for pin in range(4):
            v, d = resp[2 + 2 * pin], resp[3 + 2 * pin]
            out.append(None if v == NOT_GPIO else (bool(v), bool(d)))
        return out

    # --- I2C (Tables 3-20 to 3-31) -----------------------------------------------------

    def _wait_write_done(self) -> None:
        for _ in range(POLL_LIMIT):
            st = self.status()
            if st[8] == 0:
                if st[20] & 0x40:
                    raise I2cNack("no ACK from the I2C device")
                return
            self._sleep(0.001)
        self.cancel_i2c()
        raise I2cNack(f"I2C transfer did not finish (engine state 0x{st[8]:02X})")

    def _start(self, cmd: int, addr7: int, length: int, read: bool, data: bytes = b"") -> None:
        if not 0x08 <= addr7 <= 0x77:
            raise ValueError(f"I2C address 0x{addr7:02X} outside 0x08-0x77")
        resp = self._xfer(cmd, length & 0xFF, length >> 8, (addr7 << 1) | int(read), payload=data)
        if resp[1] != 0x00:
            self.cancel_i2c()
            raise Mcp2221Error("I2C engine busy; transfer cancelled")

    def i2c_write(self, addr7: int, data: bytes, stop: bool = True) -> None:
        if not data or len(data) > I2C_CHUNK:
            raise ValueError(f"write 1-{I2C_CHUNK} bytes per call")
        self._start(CMD_I2C_WRITE if stop else CMD_I2C_WRITE_NO_STOP, addr7, len(data), False, data)
        self._wait_write_done()

    def i2c_read(self, addr7: int, n: int, restart: bool = False) -> bytes:
        if not 0 < n <= I2C_CHUNK:
            raise ValueError(f"read 1-{I2C_CHUNK} bytes per call")
        self._start(CMD_I2C_READ_RESTART if restart else CMD_I2C_READ, addr7, n, True)
        for _ in range(POLL_LIMIT):
            resp = self._xfer(CMD_I2C_GET_DATA)
            if resp[1] == 0x00 and resp[3] != 127:
                if resp[3] != n:
                    raise I2cNack(f"short read: {resp[3]} of {n} bytes")
                return bytes(resp[4 : 4 + n])
            if resp[3] == 127 and resp[1] == 0x00:
                break
            self._sleep(0.001)
        self.cancel_i2c()
        raise I2cNack(f"no data from I2C device 0x{addr7:02X}")

    def i2c_write_read(self, addr7: int, data: bytes, n: int) -> bytes:
        """Register read: write without STOP, then repeated START and read."""
        self.i2c_write(addr7, data, stop=False)
        return self.i2c_read(addr7, n, restart=True)

    def probe(self, addr7: int) -> bool:
        try:
            self.i2c_read(addr7, 1)
            return True
        except I2cNack:
            return False

    # --- Reset (Table 3-40) ------------------------------------------------------------

    def reset(self) -> None:
        """Re-enumerates the chip. The only command with no response."""
        self._hid.write(bytes([CMD_RESET, 0xAB, 0xCD, 0xEF]).ljust(REPORT, b"\x00"))
