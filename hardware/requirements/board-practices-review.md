# Board good-practice review: lights, debug aids and the rest (2026-11-20)

This is a list for the owner to rule on. **Nothing here is applied yet.** Each item carries
my recommendation:

- **DO**: recommended for Rev A.
- **OPT**: worth it if cheap or if you want it.
- **SKIP**: recommend against, with the reason.

Every part named here still needs a JLCPCB stock check (`jlc_sourcing.py`) and a datasheet check
before it goes into the generator.

## What the boards already have

| Board | Lights | Debug and test aids |
|---|---|---|
| TL-SN-001 (main) | D1 red status LED on GPIO8 (active low, 1 k, about 1.3 mA) | RESET and BOOT buttons. USB-C Serial/JTAG. DNP UART0 header J8. 27 test pads (rails, charger, I²C root, UART0, USB). DNP 0 Ω meter links JP1/JP2 (SYS and 3V3). Three fiducials. Name, revision and serial box on silk. |
| TL-MOD-001..004 (cards) | none | V3V3_AON, GND and WAKE/INT test pads. Each card has a 24AA025E48 ID EEPROM. |
| TL-DBG-001 (debug adapter) | D1 "card seated" LED | 19 test pads. AON and SW meter jumpers. MCP2221 bridge. |
| TL-TEST-001 (slot tester) | none | 7 test pads. Loopbacks and a 330 Ω load on SW. |

Gaps found while surveying:

- **No board has mounting holes.**
- **The ESP32-H2 has no free GPIO.** All 32 expander pins are used too (U15 and U16). Any new
  firmware-driven light on the main board therefore needs a new driver part.

## 1. Lights

Rules I'd follow on every board:

1. **Battery boards stay dark by default.** A node that blinks on motion tells an intruder they
   have been seen, and every lit mA comes out of a µA budget. Lights are for bench, install and
   service. Firmware turns them on only when USB is present or a service mode is active, and
   times them out.
2. **Lights that are always on take their power from the source they report, never from the
   battery.** For example, a USB LED hangs off USB_VBUS, so it is free on battery.
3. **Never put an LED on a rail you meter.** An LED on V3V3_AON_n would add more current than a
   card's whole sleep budget and corrupt the PPK2 numbers.
4. **Use high-efficiency LEDs at 0.5–1 mA.** They are bright enough behind a light pipe. One
   colour per meaning; for the status LED, meaning comes from a blink-code table.
5. **Every light gets a silk label** (`PWR`, `STAT`, `S0`…`S3`, `FLT`). Where a production build
   omits a light, its footprint and resistor are marked DNP so the same board serves both.

### TL-SN-001 (main board)

| ID | Item | Rec | Why / cost |
|---|---|---|---|
| L-01 | Keep D1 STATUS. Write a blink-code table (boot, joining Thread, joined, OTA, low battery, fault, card error) | **DO** | Free. Firmware already owns GPIO8. Pulses of 20 ms or less keep the average current negligible. GPIO8 is a strap pin, so the active-low LED with its pull-up has to stay as it is. |
| L-02 | USB-power LED: green, from USB_VBUS to GND through about 3.3 kΩ (about 1 mA) | **DO** | Hardware only, no GPIO. It costs nothing on battery. It answers "is it getting USB" at a glance. |
| L-03 | A third TCA9535 (0x22, a part we already source) with four slot LEDs `S0`…`S3`, powered from V3V3_CORE and sunk by the expander. Firmware: off on battery, solid = card enumerated, blink = fault or quarantine, flash = wake event | **DO** (LEDs DNP-able) | Makes the four-slot logic visible at the bench. Cost: one TSSOP-24, 4 LEDs, 4 resistors, about 1 µA expander standby. **Bonus:** its 12 spare pins close HD-032 (tamper switch input) and HD-034 (wake-fault visibility) without another part. |
| L-04 | Charge-state light | OPT (firmware, no part) | BQ25186 has no STAT pin. Show "charging" as a status-LED pattern while USB is present. An LED hung on CHG_PG_N would also load an expander input, so that option needs a datasheet check. |
| L-05 | Fault light on SW_FAULT_N, or rail-on LEDs on AON/SW, hard-wired | **SKIP** | Constant drain on battery, and it corrupts current metering (rule 3). Firmware shows faults on the slot LEDs instead. |
| L-06 | RGB status LED instead of red D1 | SKIP for Rev A | Needs 3 drivers and adds BOM. Revisit with the enclosure and light-pipe design. |

### TL-DBG-001 (debug adapter, USB powered, so no battery limit)

| ID | Item | Rec | Why / cost |
|---|---|---|---|
| L-07 | `PWR` LED on V3V3 (after the AP2112) | **DO** | Shows the bridge is alive. About 1 mA from USB. |
| L-08 | `AON` and `SW` LEDs driven by the enable nets DBG_AON_EN and DBG_SW_EN (LED plus about 2.2 kΩ to GND) | **DO** | Shows what the bench told the card to power. These LEDs sit on the bridge side, not on the metered rails, so PPK2 numbers are unaffected. The MCP2221 GP pins source enough current for 1 mA; verify against the datasheet. |
| L-09 | `FLT` LED on DBG_SW_FAULT_N (from V3V3) | **DO** | A TPS2553 current-limit trip is otherwise invisible. Here it lights at once. |
| L-10 | UART RX/TX activity LEDs | SKIP | The MCP2221 LED_URx/UTx functions sit on GP pins that are already used as enables. |

### TL-TEST-001 and the cards

| ID | Item | Rec | Why / cost |
|---|---|---|---|
| L-11 | TL-TEST-001: green LED with its own resistor on V3V3_SW | OPT | Visual "slot switched on". It adds about 1 mA to the 10 mA load, so the bench thresholds need updating. |
| L-12 | LEDs on the sensor cards (for example a PIR walk-test light) | **SKIP** | Cards have no spare drive pin. The PIR output is current-limited and must not be loaded. An LED inside a card also gives away detection. Do walk-test on the main board's status or slot LEDs instead. |

## 2. Debug and bring-up aids

| ID | Board | Item | Rec | Why / cost |
|---|---|---|---|---|
| D-01 | SN | Tag-Connect TC2030-NL footprint carrying 3V3, GND, U0TXD, U0RXD, CHIP_EN and BOOT_N | **DO** | Bare pads plus 3 holes: no part to buy and smaller than J8. It allows pogo-pin flashing and recovery if USB is dead or the USB-JTAG pins get reconfigured. Keep J8 as DNP or remove it in favour of this. |
| D-02 | SN | Test pads on each slot rail: V3V3_AON_0..3 and V3V3_SW_0..3 | **DO** | Today you can't probe a slot rail without a card on the debug board. 8 pads. |
| D-03 | SN | Test pads on SLOT_WAKE0..3, SPI_SCK/MOSI/MISO and VBAT_SENSE | **DO** | Wake-line and shared-SPI issues (HD-027, HD-034) are exactly what we will chase on the bench. |
| D-04 | SN | DNP 2×5 1.27 mm logic-analyzer header (I²C root, SPI, UART1, EXP_INT_N, GND) | OPT | One clip for a Saleae instead of 8 probes. Skip it if D-03 is done and board area is tight. |
| D-05 | SN | Net names next to every test pad on silk, or at least on the assembly/fab drawing | **DO** | silk_pass already hides refs that don't fit. Add a test-pad map to the fab PDF so the names live somewhere. |
| D-06 | SN | Large GND hook points (2 × THT loops, e.g. Keystone 5001) near the power stage and the MCU | OPT | Scope ground clips on 1 mm pads pop off. Needs a JLCPCB check for THT. |
| D-07 | DBG | Same 2 × GND loops, plus a 2-pin header in series with each metered rail (already JP1/JP2) | **DO** (loops) | Bench board: probing comfort matters more than size. |
| D-08 | SN | Current-sense links per slot (DNP 0 Ω in series with each AON/SW switch output) | OPT | 8 more 0402s. Lets you meter one card in place without the debug board. The DBG board already meters a card on its own. |
| D-09 | SN | Board revision readable by firmware | **DO** (software) | No GPIO or ADC is free for a strap. Write board, revision and serial into the existing `talos_id` partition at program time, and make the bench check it. |
| D-10 | all | Boot log and diag console documented next to the board (baud rate, strap meanings, buttons) | **DO** (doc) | One table in docs/hardware/bench-test-plan.md, matching the silk labels. |

## 3. Mechanical and assembly

| ID | Board | Item | Rec | Why / cost |
|---|---|---|---|---|
| M-01 | SN | **Mounting holes:** 4 × M2.5 plated, tied to GND through a 0 Ω/ESD path, or left isolated (owner/enclosure call) | **DO** | The board has none, so it can't be fixed in an enclosure and the sockets take all the insertion force. The 80 × 70 placement must re-run with 4 keep-outs. **This is the biggest gap in this list.** |
| M-02 | DBG | 4 × M3 holes for standoffs or rubber feet | **DO** | It is a bench board that gets cables yanked. |
| M-03 | TEST, cards | Mounting holes | SKIP | They are held by the socket. Card retention belongs to HD-025 and HD-040 (keying and edge). |
| M-04 | all | Reserve a spot for the JLCPCB order number ("JLCJLCJLCJLC" text) on bottom silk | **DO** | Otherwise JLCPCB prints it wherever it likes, possibly over parts or labels. |
| M-05 | cards, DBG, TEST | Local fiducials (they rely on panel-rail fiducials today) | OPT | Only matters if they are assembled as single boards. Decide with the HD-040 panel plan. |
| M-06 | SN | Light-pipe position for D1 and L-02 agreed with the enclosure | **DO** (when the enclosure starts) | Put the LEDs together on one edge now, so a single light pipe works later. |

## 4. Silkscreen and labelling

| ID | Item | Rec | Why |
|---|---|---|---|
| S-01 | Connector labels: `USB`, `PV+ / −`, `BAT+ / NTC / −`, `SLOT 0..3` next to each socket | **DO** | Labels next to connectors are the cheapest way to prevent a wrong plug-in. Battery polarity is HD-024. |
| S-02 | Pin-1 and polarity marks on every IC, diode, LED and polarised connector, visible after assembly | **DO** | silk_pass places refs; it doesn't yet check that pin-1 marks survive. Add that as a check. |
| S-03 | Button labels `RST` / `BOOT`; LED labels per L-rules | **DO** | |
| S-04 | Card face: card type and the orientation arrow "this side up / toward board" | **DO** | It mitigates HD-025 until the keying is fixed. |
| S-05 | Licence and URL text, "Li-ion: use protected pack" warning | OPT | Open-source hardware convention. |

## 5. Electrical good practice not yet covered by HD items

| ID | Item | Rec | Note |
|---|---|---|---|
| E-01 | Input protection on the debug board's card rails (it powers unknown cards) | OK as is | The TPS2553 limits SW. AON comes from the TPS22917 with no limit, but the bench budget is small. Note only. |
| E-02 | A polyfuse or eFuse on USB_VBUS on the SN | OPT | The USB source limits current, and the TPS2121 has ILIM. Belt and braces for a shorted card on USB. |
| E-03 | Unused pins on the TCA9535 of TL-TEST-001 tied low through 10 k (already done) | done | Listed for completeness. |
| E-04 | Spare footprints: a DNP bulk capacitor on SYS_PATH and a DNP filter on PV_IN | OPT | Cheap insurance against bench surprises (charger stability, PV noise). |

## My suggested Rev A bundle

- **Lights:** L-01, L-02, L-03 (third expander, which also unblocks HD-032 and HD-034), and
  L-07 to L-09.
- **Debug:** D-01, D-02, D-03, D-05, D-09 and D-10.
- **Mechanical:** M-01, M-02 and M-04.
- **Silkscreen:** S-01 to S-04.

The bundle costs about 1 TSSOP-24 and 12 LEDs and resistors across the boards, plus the
mounting-hole re-placement. Every item here is a schematic, pin or mechanical change, so each
needs the owner's yes before it goes into the generators. After that, the main board needs a
full re-place and re-route.
