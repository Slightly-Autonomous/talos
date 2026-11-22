"""Module codes stored in each card's ID EEPROM (ID block v1, see id_block.py).

Mirror of the firmware registry in firmware/portable/modules.h and modules.cc.
tests/test_contracts.py fails if the two drift apart or a manifest is missing.
"""

from __future__ import annotations

MODULES: dict[int, str] = {
    0x0101: "pir_ekmb_v1",
    0x0102: "reed_contact_v1",
    0x0103: "tl_mod_001_magnetic",
    0x0104: "tl_mod_002_pir",
    0x0105: "tmag5273a2_magnetic",  # TL-SN-002 built-in (no ID EEPROM, ADR 0019)
    0x0106: "pir223d_motion",  # TL-SN-002 built-in
    0x0201: "vibration_lis2dw12_v1",
    0x0202: "tl_mod_003_imu",
    0x0301: "environment_sht40_v1",
    0x0302: "tl_mod_004_environment",
    0x0401: "smart_camera_s3_v1",
    0xF001: "tl_test_001",
}

# Rev A card -> module code, for programming card IDs.
CARDS: dict[str, int] = {
    "TL-MOD-001": 0x0103,
    "TL-MOD-002": 0x0104,
    "TL-MOD-003": 0x0202,
    "TL-MOD-004": 0x0302,
    "TL-TEST-001": 0xF001,
}


def manifest_for(code: int) -> str | None:
    return MODULES.get(code)
