# ADR 0016: Hardware bring-up path and HIL testing

**Status:** Accepted (2026-11-16). Starts Phase 8 (spec §49-50).

## Decision
- **Dev kit first, custom PCB later.** Bring-up uses an ESP32-H2-DevKitM-1, a TCA9546A, ID EEPROMs and module breakouts (`hardware/bom/devkit_rig_v0.csv`), running the **same** `firmware/portable` code the twin tests. Custom PCB layout waits until the P0 questions are answered with measurements (spec §51 "dependency principle"). The questions: pin map, wake lines, sleep current, OTA fit and connector.
- **A stable serial contract:** the ESP-IDF app logs `talos boot epoch=… version=…`, one `talos slot N module=0x… enabled=… fault=…` line per slot, and periodic `talos alive …` lines. Bench checks and HIL tests parse only these lines.
- **HIL tests** (`tests/hil`, `pytest-embedded` 1.17.1) check what simulation cannot: the real boot, NVS-persisted boot epoch across a hard reset, and discovery on real I2C. They run **only** from a manually dispatched workflow (`.github/workflows/hil.yml`) on a self-hosted runner labeled `talos-hil`, restricted to this repository. Pull requests never reach lab hardware, and the job uses no signing keys or secrets (spec §43).
- **Release discipline:** `docs/manufacturing/release_package.md` lists what a hardware release must contain. Manifest values become non-provisional **only** when measured (spec §56 statuses).

## Status (honest)
- **None of this has run:** no dev kit was available while writing it. The HIL tests are syntax-checked only; the ESP-IDF build compiles in CI.
- **Still to do:**
  - Thread (OpenThread CoAP client as `IDatagram`, plus OTBR)
  - sleep and wake interrupts
  - battery ADC and the `esp_ota` flash binding
  - secure boot staging with owner keys
  - the KiCad mainboard and connector selection
  - factory fixtures and certification pre-compliance
