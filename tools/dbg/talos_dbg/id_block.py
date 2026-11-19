"""Card ID block v1, the same layout firmware/portable/modules.h reads.

Page 0 (16 bytes) of the 24AA025E48:
    0-1  module code, big-endian
    2    hardware revision
    3    format (0x01)
    4    variant
    5-14 reserved, 0xFF
    15   CRC-8 (poly 0x31, init 0xFF) over bytes 0-14
The EUI-48 at 0xFA-0xFF is the card's serial number. Only 0x00-0x7F is
writable; the upper half is write-protected in silicon (DS22124D).
"""

from __future__ import annotations

from dataclasses import dataclass

FORMAT_V1 = 0x01
SIZE = 16
PAGE = 16
EUI_ADDR = 0xFA
WRITABLE_END = 0x80


def crc8(data: bytes) -> int:
    crc = 0xFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def encode(code: int, revision: int, variant: int = 0) -> bytes:
    if not 0 < code < 0xFFFF or not 0 <= revision <= 0xFF or not 0 <= variant <= 0xFF:
        raise ValueError("code 1-0xFFFE, revision and variant 0-255")
    body = bytes([code >> 8, code & 0xFF, revision, FORMAT_V1, variant]) + b"\xFF" * 10
    return body + bytes([crc8(body)])


@dataclass(frozen=True)
class IdBlock:
    status: str  # "ok", "blank" or "corrupt"
    code: int
    revision: int
    variant: int
    raw: bytes


def decode(raw: bytes) -> IdBlock:
    if len(raw) != SIZE:
        raise ValueError("ID block is 16 bytes")
    code = (raw[0] << 8) | raw[1]
    if raw == b"\xFF" * SIZE:
        status = "blank"
    elif raw[3] != FORMAT_V1 or crc8(raw[:15]) != raw[15] or code in (0, 0xFFFF):
        status = "corrupt"
    else:
        status = "ok"
    return IdBlock(status, code, raw[2], raw[4], bytes(raw))


def eui_problems(eui: bytes) -> list[str]:
    """Empty when the EUI-48 looks like a real factory-programmed address."""
    problems = []
    if eui in (b"\x00" * 6, b"\xFF" * 6):
        problems.append("EUI-48 is blank (wrong part, or a plain 24AA025 without EUI)")
    elif eui[0] & 0x01:
        problems.append("EUI-48 is a multicast address")
    elif eui[0] & 0x02:
        problems.append("EUI-48 is locally administered, not a factory address")
    return problems


def eui_text(eui: bytes) -> str:
    return "-".join(f"{b:02X}" for b in eui)
