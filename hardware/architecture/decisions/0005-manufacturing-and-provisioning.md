# HW-0005: Manufacturing and provisioning boundaries

**Status:** Accepted for H0–H3. The detailed factory flow comes at H4/H6.

## Decision
- **Turnkey assembly with no hand soldering.** Components go on one side where practical. A 2-layer board is the cost starting point; 4 layers are allowed if RF or ground review demands it (HD-009).
- **Factory test runs through pads**, not populated headers:
  - V3V3_CORE, GND, CHIP_EN, GPIO9/BOOT
  - U0TXD/U0RXD
  - USB D+/D−
  - VBAT, SYS_PATH
  - root I²C
  
  TL-TEST-001 cartridges exercise all four slots at once.
- **Identity:** the per-unit identity comes from the module eFuse MAC and the card EEPROM (24AA025E has a factory EUI-48). Device keys are generated at claim time on the device, and are never stored in the factory log or the repo (ADR 0014, spec §37).
- **Nothing irreversible without owner approval.** No secure-boot or flash-encryption eFuses are burnt before the open-source owner-signing and recovery policy is decided (HD-016). No orders, binding quotes or factory keys without explicit owner approval.

## Consequences
`docs/manufacturing/release_package.md` stays the checklist for a hardware release. The KiCad projects are generated and must be regenerated (never hand-edited) until layout starts.
