# Bench kit for the first three TL-SN-002 boards (2026-11-22)

The goal (owner): plug the boards into the development computer, then let Claude debug, test and change
things, and gather what Rev B needs. Everything below is driven from the bench tools in this
repository, `tools/bench` and `tools/hil`. Prices are rough 2026 street prices, for orientation only.

## Must have

| Item | Why | Used by |
|---|---|---|
| **Nordic Power Profiler Kit II (PPK2)** | Powers the board through its battery terminals as 2 × AA, at any voltage from 0.8 to 5 V, and measures 200 nA–1 A at 100 kS/s. It verifies the deep-sleep floor (HWR-009), the TX burst and the low-battery threshold (HD-043). About $100. | `python3 -m talos_bench.power --port …` |
| **Powered USB hub with per-port power switching** that `uhubctl` supports (check the uhubctl compatibility list) | Lets the bench runner power-cycle each board over USB: cold boots, the USB-reset report, brown-outs. No unplugging. About $40–80. | `tools/bench` (power-cycle step) |
| **802.15.4 radio for the hub (RCP)**: one ESP32-H2-DevKitM-1, flashed with ESP-IDF's `ot_rcp` | The hub's Thread border router side, needed to test the boards over Thread instead of the USB console. About $10. | OTBR + `talos_hub` |
| **A small Linux box for the border router**, such as a Raspberry Pi 5 with 4 GB | OpenThread Border Router runs on Linux. Docker on macOS can't pass the RCP's USB through. The Pi runs OTBR and the hub; the Mac drives the bench. About $80 with PSU and card. | `deploy/` |
| **Tag-Connect TC2030-IDC-NL cable** plus a 3.3 V USB-UART adapter | Recovery console on UART0 (J10) if USB-C or the firmware is broken. About $40 + $10. | ESP-IDF `idf.py monitor`, `esptool` |
| **Energizer L91 AA lithium (8+)**, plus a few alkaline AAs | The design cell, plus the worst case for sag (HD-043). | power bench |

## Strongly recommended

| Item | Why |
|---|---|
| **USB logic analyzer** (8 channels; a Saleae Logic 8, or a sigrok-compatible fx2lafw clone at about $15) | I²C traffic, wake-line timing, PIR output and tamper debounce against the firmware's numbers. |
| **N42 10 × 3 mm disc magnets (10)**, plus one strong N52 test magnet (about 20 mm) | The door magnet the carrier holds, and the foreign magnet for the magnet-defeat test (`tamper.magnet`). |
| **USB-C power meter** (inline VBUS volts and amps) | USB current and switchover (TPS2116). |
| **Thermometer with a thermocouple, plus a freezer (−18 °C)** | Cold cells and the holder (HD-043, HD-046), cold actuation of the tamper switches and the PIR's −20 °C limit (HD-049, HD-044). |

## For the housing test print (Bambu P2S)

| Item | Why |
|---|---|
| M2.5 heat-set inserts (OD about 3.9, L 4) and M2.5 × 6 screws | Board to base. |
| M3 × 12 screws (and M3 inserts if the lid comes off often) | Lid. |
| PETG and ASA filament, TPU 95A, plus a short length of **clear** 1.75 mm PETG | Housing; gasket; the light pipes are offcuts of the clear filament. |
| 2–3 HDPE Fresnel lenses, 15 mm flange, multi-zone (see the housing README) | PIR window. Pick the one that passes the bench. |
| #8 wood screws, #4 screws, a door frame offcut or the real door | Mounting and the magnet gap. |

## What happens when they arrive

1. Plug each board into the hub over USB-C. Claude runs `python3 -m talos_bench.flash`, which flashes
   the firmware, writes the board identity and runs the self-test. Then `talos_bench --board TL-SN-002`
   runs the bench plan, with the lid-open steps on prompt.
2. Power each board from the PPK2 on its battery terminals and run `talos_bench.power`.
3. Provision Thread (`thread <dataset> <hub-ipv6>`, lid open) against the Pi's border router, then
   measure attach and the deep-sleep wake-to-hub latency on hardware. Compare with
   `tools/otns/thread_latency.py`.
4. Fit the test print: finger preload, door slide, PIR lens, magnet gap.
5. Every finding goes into `hardware/requirements/design-risks.md` and the Rev B list.

Production security (Secure Boot, flash encryption, NVS encryption, anti-rollback) burns eFuses. Do it
on **one** board at most, and only after the others have passed. A provisioned board can only ever run
images signed with its key (`docs/hardware/provisioning.md`).
