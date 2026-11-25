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

## Vision Node, first build (ADR 0020): two cameras, one a spare

All bought parts; nothing for JLCPCB. Quantities are for two cameras.

| Item | Qty | Why | Rough price |
|---|---|---|---|
| **Raspberry Pi Zero 2 W** (no headers) | 2 | Compute with hardware H.264 encoding | $15 each |
| **Raspberry Pi Camera Module 3 Wide NoIR** (IMX708, 120°) | 2 | Wide view with autofocus that sees 850 nm IR at night | $35 each |
| **Raspberry Pi Zero camera cable** (22-pin to 15-pin, 50–100 mm) | 2 | The camera's own cable doesn't fit the Zero | $3 each |
| **Waveshare PoE/ETH/USB HUB HAT for Zero** (SKU 19635) | 2 | 802.3af PoE into isolated 5 V 2.5 A, plus 100 Mbit Ethernet (RTL8152B) | $25 each |
| **802.3af PoE switch**, 4+ ports (or one 802.3af injector per camera) | 1 | Powers the cameras and connects them to the LAN, the hub and the Mac | $40–60 |
| **850 nm IR LED board pair with photoresistors** (the common 3 W pair sold for Pi cameras) | 2 pairs | Night light; they switch themselves on in the dark | $8 a pair |
| **Aluminium heatsink for the Zero 2 W** (adhesive, low profile) | 2 | VB-01 thermals in a closed housing | $3 each |
| **High-endurance microSD, 32 GB** (SanDisk High Endurance or Samsung PRO Endurance) | 3 | System card, plus a spare for re-flash tests (VB-06) | $10 each |
| **Lever microswitch** with a normally-open contact (e.g. Omron SS-5GL or D2F-L class); the lid presses it closed | 4 | Lid tamper on GPIO17 | $2 each |
| **Clear cast-acrylic disc**, 30 mm × 2 mm | 8 | Three windows per camera (the lens and each IR board), plus spares; clear silicone to seal them | $8 for ten |
| **Closed-cell foam strip**, 3 mm | 1 | Light baffle between the IR LEDs and the lens (stops IR glare in the window) | $5 |
| **Short Cat6 patch cables**, 0.5–1 m | 3 | Bench | $10 |
| **Jumper wires** (female) and a 5 V 2.5 A micro-USB supply | 1 set | Tamper switch and IR power from the header; bench power without PoE | $15 |

Housing (Bambu P2S, `hardware/mechanical/vision_node/`): ASA; M2 × 5 screws (camera, IR boards); M2.5
inserts and screws (Pi stack); four M3 × 10 screws per lid; M5 × 30 bolt, washer and wing nut (mount);
and a PG9 cable gland with a split insert for the Ethernet cable, so the plug doesn't have to come off. Frigate runs on this Mac in Docker for now, so the hub side needs
nothing new.

## What happens when they arrive

1. Plug each board into the hub over USB-C. Claude runs `python3 -m talos_bench.flash`, which flashes
   the firmware, writes the board identity and runs the self-test. Then `talos_bench --board TL-SN-002`
   runs the bench plan, with the lid-open steps on prompt.
2. Power each board from the PPK2 on its battery terminals and run `talos_bench.power`.
3. Provision Thread (`thread <dataset> <hub-ipv6>`, lid open) against the Pi's border router, then
   measure attach and the deep-sleep wake-to-hub latency on hardware. Compare with
   `tools/otns/thread_latency.py`.
4. Fit the test print: finger preload, door slide, PIR lens, magnet gap.
5. Vision Node: flash an SD card with `tools/provision/vision_node.py`, plug the camera into the PoE
   switch, and Frigate on the Mac picks it up. Then run VB-01..08
   (`hardware/architecture/vision-compute-study.md`).
6. Every finding goes into `hardware/requirements/design-risks.md` and the Rev B list.

Production security (Secure Boot, flash encryption, NVS encryption, anti-rollback) burns eFuses. Do it
on **one** board at most, and only after the others have passed. A provisioned board can only ever run
images signed with its key (`docs/hardware/provisioning.md`).
