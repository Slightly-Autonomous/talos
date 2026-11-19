# Dev-kit bring-up (Phase 8, step 1)

This covers the ESP32-H2 dev kit. The Rev A board sequence, pass limits and fault trees are in [bench-test-plan.md](bench-test-plan.md).

Goal: run the **same portable firmware** on real silicon with off-the-shelf breakouts, before any custom PCB. That answers the P0 hardware questions (pin map, wake lines, I2C behavior, sleep current, OTA fit) with measurements instead of guesses. See ADR 0016.

## 1. Wire the rig (`hardware/bom/devkit_rig_v0.csv`)
- I2C SDA/SCL to GPIO2/GPIO3 and slot wakes to GPIO10–13, per `firmware/esp_idf/main/board_tl_sn_001.h`. These follow the sourced pad ledger `hardware/pins/esp32h2_mini1_pin_ledger.csv`. On a DevKitM-1, check those GPIOs are broken out and free before wiring. **First bench task: HD-004**, deep-sleep wake on GPIO10–13 with mixed levels.
- A TCA9546A at 0x70. Branch *i* goes to slot *i*.
- On each used branch, an EEPROM at 0x50 holding ID block v1 in bytes 0–15: code (big-endian), revision, format `0x01`, variant, ten `0xFF`, then CRC-8 (poly 0x31, init 0xFF) over bytes 0–14 (`fw::make_id_block`). A blank or corrupt block is reported as a fault, never as an empty slot.

  | Module | Code |
  |---|---|
  | PIR (dev kit) | `0x0101` |
  | Reed contact | `0x0102` |
  | TL-MOD-001 3-axis Hall contact (TMAG5273) | `0x0103` |
  | TL-MOD-002 PIR card | `0x0104` |
  | LIS2DW12 (dev kit) | `0x0201` |
  | TL-MOD-003 IMU card | `0x0202` |
  | SHT40 (dev kit) | `0x0301` |
  | TL-MOD-004 environment card | `0x0302` |
  | Camera | `0x0401` |
  | TL-TEST-001 slot test card | `0xF001` |

- Slot wake lines (PIR output, reed, LIS2DW12 INT1) go to the GPIOs in `proto_a()`.

## 2. Flash and watch the serial log

```bash
cd firmware/esp_idf && idf.py set-target esp32h2 build flash monitor
```

Expected:

```
talos boot epoch=1 version=1
talos slot 0 module=0x0101 enabled=1 fault=0
talos slot 1 module=0x0201 enabled=1 fault=0
talos slot 2 module=0x0000 enabled=0 fault=0
talos slot 3 module=0x0301 enabled=1 fault=0
talos alive queued=3 mode=0
```

`queued` grows because the Thread link is still a stub. That is expected until step 4.

## 3. Run the HIL tests
On a machine with the kit attached (or the self-hosted runner in `.github/workflows/hil.yml`):

```bash
pip install -r tests/hil/requirements.txt
```

```bash
pytest tests/hil -c tests/hil/pytest.ini --embedded-services esp,idf --target esp32h2 --app-path firmware/esp_idf --port /dev/ttyUSB0
```

Set `TALOS_RIG=1` to include the wired-rig discovery test.

## 4. Measure and record (feeds the manifests and twin)

| Question (spec §38) | Measure | Update |
|---|---|---|
| Sleep current with 4 slots | PPK2, idle with PIR + IMU armed | battery budget in board manifest |
| Wake latency per slot | scope: wake line → first log line | twin PIR/contact timing |
| Contact bounce on real reed | scope on the line | `ContactDriver::kDebounceMs` |
| LIS2DW12 threshold vs. glass strikes | strike tests on a real window | default `impact_threshold_mg` |
| OTA image size | `idf.py size` | partition table / 4 MB decision |
| I2C mux branch isolation | short one branch | already covered in sim; confirm on silicon |

Record results in `docs/decisions/open-decisions.md`, then remove `provisional` only for numbers you actually measured.

## 5. Remaining firmware work for real nodes
- The **Thread link:** OpenThread CoAP client implementing `IDatagram` → `CoapLink` (already tested over UDP), plus OTBR on the hub host.
- **Sleep:** GPIO wake interrupts and light/deep sleep driven by `next_wake_ms()`.
- **Hardware glue:** battery ADC, `esp_ota_*` binding for `IFlash`, and Secure Boot / flash-encryption staging with owner keys. Do **not** burn eFuses until key management is decided.
