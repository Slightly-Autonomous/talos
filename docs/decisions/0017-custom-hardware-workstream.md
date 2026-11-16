# ADR 0017: Custom hardware workstream (H0–H3)

**Status:** Accepted (2026-11-16). Adopts `TALOS_CUSTOM_HARDWARE_HANDOFF_v0_1` as the hardware plan. It refines ADR 0016: dev-kit bring-up is still the first physical step.

## Decision
- **The hardware workstream lives in `hardware/`**, with its own ADRs HW-0001..0005 in `hardware/architecture/decisions/`. Root ADRs win if the two conflict.
- **Board family:**
  - the hub is bought (TL-HUB-REF)
  - one bespoke four-slot Thread Sensor Node (TL-SN-001)
  - a separate Vision Node (TL-VN-001)
  - four reference cards and a slot test cartridge
  
  The camera slot module is superseded (HW-0001).
- **Pins come from a sourced ledger.** `hardware/pins/esp32h2_mini1_pin_ledger.csv`, built from the ESP32-H2-MINI-1 datasheet v1.6, is the only source for `firmware/esp_idf/main/board_tl_sn_001.h`. It replaces the placeholder `board_proto_a.h`, whose wake pins 0/1/4/5 cannot wake from deep sleep. Manifests still carry no pins.
- **The manifest schema gains:**
  - `connector_standard`
  - `uart_channels`
  - `reserved_i2c_addresses`
  - `board_family` and `transport`
  - µW-resolution budgets (`sustained_uw`, `average_uw`)
  
  The validator adds `kConnectorMismatch`, `kI2cRootConflict` and `kUartOversubscribed`. The handoff §38 cases HW-MF-001..009 are tests in `//codec:yaml_loader_test`.
- **KiCad sources are generated** by `hardware/scripts/gen_schematics.py` and checked by `hardware/scripts/check_kicad.sh` (ERC plus a netlist round-trip). This stays true until layout starts.

## Not decided here
Connector MPN, charger values, battery pack, wake semantics on silicon, and everything in the Vision Node. See `hardware/requirements/design-risks.md`.
