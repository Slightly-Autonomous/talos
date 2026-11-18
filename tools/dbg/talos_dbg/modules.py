"""Module codes stored in each card's ID EEPROM (bytes 0-1 big-endian code, byte 2 revision).

Mirror of the firmware registry in firmware/portable/modules.h and modules.cc. test_modules.py
fails if the two drift apart.
"""

from __future__ import annotations

MODULES: dict[int, str] = {
    0x0101: "pir_ekmb_v1",
    0x0102: "reed_contact_v1",
    0x0103: "tl_mod_001_magnetic",
    0x0201: "vibration_lis2dw12_v1",
    0x0301: "environment_sht40_v1",
    0x0401: "smart_camera_s3_v1",
}


def manifest_for(code: int) -> str | None:
    return MODULES.get(code)
