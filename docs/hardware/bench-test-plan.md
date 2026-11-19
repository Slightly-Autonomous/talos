# Rev A bench test plan

How every Rev A board, chip and firmware path gets tested, on the host and on the bench, so that any fault can be
isolated to one part, one joint, or one line of firmware. Written 2026-11-18 from a per-chip review of all seven
boards (one reviewer per chip, plus board debuggability, firmware/twin/HIL). Nothing here has run on hardware yet.

`interface-test-matrix.md` (IT-01..IT-10) stays the list of slot-interface requirements. This plan says how they and
everything else get tested, in what order, and what is still missing to do it.

## 1. What the review changed and found

### Fixed now (in the same change as this plan)

| Item | Was | Now | Why |
|---|---|---|---|
| TL-SN-001 D3 SMAJ12A | pin 1 (cathode) on GND | cathode on PV_IN | Forward-biased: it would have clamped PV_IN at ~0.7 V and shorted the panel |
| TL-SN-001 R12 TPS2121 ILIM | TBD, not placed | 100 kΩ (1.24 A typ, SLVSEA3F eq. 2) | Floating ILIM is unspecified: USB/PV might never reach the charger |
| TL-SN-001 C7 TPS2121 SS | TBD, not placed | 100 nF (~780 V/s, TI's own example) | Inrush and input debounce |
| TL-SN-001 R15 ST pull-up | 100 kΩ | 10 kΩ | Datasheet range 6–20 kΩ |
| Card placement | parts allowed on the tongue above the finger zone | nothing but tracks on the whole 8.5 mm tongue | The tongue goes into the socket body |
| SN placement | inductor orientation was arbitrary | two-pad parts flipped to face their nets | L1 forced LX2 to cross LX1 |
| SN plane fanout | every plane pin got a via | fine-pitch plane pins next to a power pin are left to the router | Those vias blocked the TPS63900 switch node and BQ25186 VBAT; replaces `finish_sn.py` |

### Firmware bugs that pass in simulation but would fail on hardware

The virtual board was kinder than the real chips in these places, so each needs a sim fix and a driver fix together. **Fixed** rows have both, with a regression test in `firmware/sim/node_test.cc` that fails on the old driver.

| ID | Where | Bug | Effect on hardware | Status |
|---|---|---|---|---|
| FW-B1 | `modules.cc` Sht40Driver::poll | reads straight after 0xFD; the SHT40 NACKs for up to 8.3 ms | every reading is `kBusError` | Fixed |
| FW-B2 | `modules.cc` Lis2dw12Driver | CTRL7 INTERRUPTS_ENABLE (needs a bench check) and CTRL3 LIR not set | interrupts are 1/ODR pulses; impacts are lost | Fixed (CTRL7 still needs a bench check) |
| FW-B3 | `modules.cc` Lis2dw12Driver | "peak" read from the low-pass path | reports ~1000 mg of gravity at rest | Fixed |
| FW-B4 | `modules.cc` Tmag5273ContactDriver | peak across all axes while only one is converted | an oblique magnet can hide an opening | Fixed |
| FW-B5 | `modules.cc` Tmag5273 init | reads before RESULT_STATUS | false "open" at boot | Fixed |
| FW-B6 | `modules.cc` PirDriver | no warm-up mask (25 s typ, 210 s max) | spurious P0 motion after every AON power-up | Fixed |
| FW-B7 | `esp_platform.cc:34` | internal pull-down on WAKE lines (~45 kΩ) | PIR output loaded to ~75 µA, near its 100 µA false-alarm limit | Fixed |
| FW-B8 | `esp_platform.cc:77-82` | `set_rail` ignores the 3V3 rails; no TCA9535 driver | SW rails never turn on; AON only on because of pull-ups | Fixed: `BoardIo` drives the expanders; rails follow S-2/S-3 |
| FW-B9 | `esp_platform.h:42`, `.cc:84` | `source()` always USB, `battery_mv()` always 3900 | node never enters low-power mode | Fixed: source from CHG_PG_N + PMUX_ST (unknown if unreadable), ADC for VBAT |
| FW-B10 | `runtime.cc:105` | any ID read failure, and code 0x0000, mean "empty slot" | dead mux, dead card, blank card all look empty; `kNoIdEeprom` never sent | Fixed: PRESENT_N separates a dead card (kNoIdEeprom) from an empty slot |
| FW-B11 | `modules.cc:30-35` | mux never deselected after a slot access | one stuck branch takes down the root bus | Fixed |
| FW-B12 | `esp_platform.h:66` | `Link::send` always false | nothing leaves a real node: no end-to-end test possible | Fixed: CoAP datagrams over the console (`@F` lines) and `tools/hil` serial bridge; tested end to end against the real hub |
| FW-B13 | `tools/dbg/talos_dbg/adapter.py:141` | ALERT read as asserted when AON is off | `status` prints a false alert; must print unknown | Open |

### Contract drift

- **Fixed:** the Rev A cards and TL-TEST-001 now have their own codes (0x0104, 0x0202, 0x0302, 0xF001) mapped to
  their own manifests; a contract test checks firmware, `tools/dbg` and the manifest files agree. `environment_sht40_v1` says the sensor is on the SW rail (it is on AON).
- `core/manifest/manifest.cc:182` rejects more than one UART card, so four TL-TEST-001 cards (IT-01) fail validation.
- The manifest lets slots draw 380 mA plus a 123 mA radio peak; the TPS63900 gives 400 mA.

## 2. Bench equipment

| Item | Need | Used for |
|---|---|---|
| Mac, TL-DBG-001, TL-TEST-001 ×4 | essential | everything |
| Nordic PPK2 + `ppk2-api` | essential | µA/mA current, battery emulation (source mode up to 5 V), logic inputs for timing |
| Multimeter with diode mode and µA range | essential | pre-power checks, IT-07 cross-check |
| Bench supply, current-limited, 0–15 V | essential | PV input tests (PPK2 stops at 5 V), short tests |
| 2–4 ch USB scope, ≥ 20 MHz | essential for power tests | TPS63900 ripple/start-up/TX droop, IT-05, IT-09 switchover |
| J3 bench cable: JST-PH 3-pin with a 10 kΩ NTC resistor, plus a switchable NTC pigtail (2.7k/4.3k/10k/20k/30k/open) | essential | BQ25186 without a real cell |
| Door magnet + non-ferrous gap fixture | essential for TL-MOD-001 | distance sweep |
| Reference T/RH meter (or a second SHT4x) | essential for TL-MOD-004 | plausibility |
| Loupe/microscope | essential | missing ESD parts are electrically invisible |
| uhubctl-capable USB hub | strongly recommended | scripting USB plug/unplug |
| Logic analyser | nice-to-have | WAKE glitches, I²C timing |
| Protected 1S Li-ion cell, fuse, LiPo bag | only after emulator tests pass | real charge test |
| RP2040 Pico on a card-edge breakout | later (HIL tier H4) | scripted fake sensor |

## 3. Test layers

1. **Host unit tests** (Bazel, pytest): drivers against register-accurate fakes, including busy-NACK, pulsed
   interrupts and every fault in §6. Run in CI.
2. **Firmware on the virtual board** (`firmware/sim`): the real `NodeRuntime` with fault injection, polled the way
   `main.cc` polls (not instantly per edge).
3. **Twin with a backend seam**: the same scenario YAML runs on behavioural devices (L1), real firmware on the
   virtual board (L2), or real hardware (L4). A parity test keeps L1 honest against L2.
4. **Bench**: `tools/bench` runs the stages in §4 over USB, records JSONL, and walks the fault trees in §5. The trees
   are tested in CI against `firmware/sim:bench_node` with injected faults before any board exists.

Result record (one line per test, kept outside the repo in `~/talos-bench/<board>/<run>.jsonl`): test id, board id
(ESP MAC or card EUI-48), firmware SHA, measured value, limits, verdict `pass|fail|blocked|skipped`, log path. A
test whose prerequisite failed is `blocked`, never `fail`, so one fault does not cascade.

## 4. Bench sequence

Order is smallest power first. **A** = fully automated from the Mac. Datasheet numbers in brackets.

### Stage 0: unpowered checks (every board, manual, meter)

| ID | Check | Pass | Isolates |
|---|---|---|---|
| P0-01 | Every rail to GND (VBAT, SYS_PATH, CHG_IN, USB_VBUS, PV_IN, V3V3_CORE; card AON/SW via TL-TEST-001 TP1/TP3) | > 1 kΩ and rising, never < 50 Ω | bridges, shorted caps |
| P0-02 | TPS63900 CFG: CFG3–GND, CFG1–GND, CFG2–GND | 16.2 kΩ ±2 %, 36.5 kΩ ±2 %, < 1 Ω | R18/R20 swapped gives a 5 V core |
| P0-03 | USB_VBUS diode mode both ways | one way 0.6 V, the other OL | USBLC6 rotated |
| P0-04 | PV_IN diode mode, red on PV_IN | OL (0.6 V means D3 reversed) | D3 orientation |
| P0-05 | PV_RAW→PV_IN diode mode | 0.15–0.35 V one way only | SS34 |
| P0-06 | Each slot pin 7/8/10/17 diode mode to GND; pins 7/8 to pin 3 | ~0.6 V one way; 4.7 kΩ | PESD orientation, pull-ups |
| P0-07 | CHIP_EN, BOOT_N with button up/down | high / 0 Ω | SW1/SW2 |
| P0-08 | Visual: every ESD part, D3 band, U-part pin 1, EKMB leads (GND = can continuity) | present and oriented | parts with no electrical signature |
| P0-09 | Card fingers: each contact to its neighbours | > 1 MΩ (PRESENT_N to GND is shorted by design) | finger bridges |

### Stage 1: TL-DBG-001 alone (it is the instrument for everything after)

| ID | Test | Pass | A |
|---|---|---|---|
| D-01 | VBUS, V3V3 at TP1 | 3.251–3.350 V [AP2112] | meter |
| D-02 | Enumerates 04D8:00DD (HID + CDC); `configure --no-card-seated`; flash read-back | match | A |
| D-03 | Line levels (status bytes 22/23) with AON off/on | 0/0 then 1/1 within 20 ms; GP3 = 1 | A |
| D-04 | Scan with AON on, empty slot | zero ACKs, no engine timeout | A |
| D-05 | AON discharge time (SDA → 0 after AON off) | record; sets the power-cycle wait | A |
| D-06 | SW at TP4 on/off | ≥ 3.2 V; < 0.1 V within 10 ms | meter |
| D-07 | Baseline AON current, PPK2 across JP1, empty slot | < 10 µA; subtract from every card | A |
| D-08 | Replug ×10 | flash survives, rails never come on | A |
| D-09 | TP10 with CDC sending 0x55, AON on/off | ~½ AON; < 0.1 V and TP3 < 0.2 V | meter |

### Stage 2: TL-TEST-001 on TL-DBG-001 (proves the fixture)

| ID | Test | Pass | A |
|---|---|---|---|
| T-01 | Scan; ID; EUI-48 read twice | exactly {0x27, 0x50}; ID matches; EUI stable, not 0/FF | A |
| T-02 | TCA9535 inputs: CS_N, RESET_N, UART_TX idle | 1, 1, 1 | A |
| T-03 | DRV_WAKE high/low → GP1; 2 ms pulse latches the GP1 interrupt flag | follows; flag set | A |
| T-04 | DRV_ALERT low → GP3 | 0 | A |
| T-05 | UART TX: CDC 300 baud 0x00 stream, poll P14 | sees 0 and 1; idle all 1 | A |
| T-06 | UART RX: P03 low 10 ms | ≥ 1 byte arrives; none when idle | A |
| T-07 | SW on, PPK2 across JP2 | 10.0 mA ±3 % (330 Ω); off < 1 µA | A |
| T-08 | AON current across JP1 | < baseline + 5 µA | A |
| T-09 | AON cycle ×20 with SW on | no USB disconnect | A |
| T-10 | AON off until SDA = 0, then on | TCA9535 config back at 0xFF (card really reset) | A |

### Stage 3: each sensor card on TL-DBG-001, then in every SN slot

Common to every card: ID programmed and read back (`id program`), scan equals the manifest's address set, AON
current within the budget line after the D-07 baseline.

| ID | Card | Test | Pass |
|---|---|---|---|
| C1-01 | MOD-001 | AON on, before init | 0.35–0.60 mA (TMAG standby 0.45 mA) |
| C1-02 | MOD-001 | Manufacturer 0x5449, DEVICE_ID & 3 = 1, DEVICE_STATUS errors 0 | exact |
| C1-03 | MOD-001 | Forced threshold (-40 mT, wake-and-sleep): WAKE without a magnet | WAKE ≥ 3.1 V within 5 ms; MAG_INT_N ≤ 0.4 V; I²C access clears it |
| C1-04 | MOD-001 | AON off | WAKE 0, AON < 0.1 V |
| C1-05 | MOD-001 | Armed current, door closed / open | ≤ 4 µA / ≤ 5 µA for the card |
| C1-06 | MOD-001 | Distance sweep, 4 orientations, both polarities | 3 mT and 1.5 mT crossing gaps recorded; offset ≤ ±1 mT |
| C2-01 | MOD-002 | Steady AON current after warm-up | EEPROM + 1 µA ±1 µA; mA means rotated: power off |
| C2-02 | MOD-002 | Warm-up time ×5 (still scene) | settles ≤ 30 s typ; fail > 210 s |
| C2-03 | MOD-002 | Hand wave at 30 cm | WAKE ≤ 0.5 s; hold 2–4 s; no retrigger in the 1.3 s mask |
| C2-04 | MOD-002 | 1 h still; then 30 min with radio TX bursts (on SN) | 0 triggers |
| C3-01 | MOD-003 | WHO_AM_I 0x44; register write/read-back | exact |
| C3-02 | MOD-003 | Power-down and LP1 12.5/50/100 Hz currents | ≤ 1.5 µA; 1/3/5 µA ×0.5–2 |
| C3-03 | MOD-003 | Static gravity, 32-sample mean | 1000 ± 60 mg |
| C3-04 | MOD-003 | Self-test both signs | every axis 70–1500 mg [DS], sign reverses |
| C3-05 | MOD-003 | Self-test step trips armed wake-up | WAKE latched until WAKE_UP_SRC read; without CTRL7 bit 5 no WAKE |
| C4-01 | MOD-004 | Soft reset, serial, CRC ×1000 | 0 CRC errors; serial stable |
| C4-02 | MOD-004 | Read header polled after 0xFD | NACK < 1 ms, ACK by 8.3 ms |
| C4-03 | MOD-004 | Idle / measuring current | ≤ 3 µA card; ≤ 500 µA peak |
| C4-04 | MOD-004 | Noise, 50 reads | T 3σ ≤ 0.1 °C, RH 3σ ≤ 0.3 %RH |
| C4-05 | MOD-004 | Heater 0x2F | ΔT ≥ +5 °C (calibrate on a golden unit), back within 0.3 °C in 30 s, no reset |
| C4-06 | MOD-004 | vs reference after 10 min soak | ΔT ≤ 0.4 °C, ΔRH ≤ 3.5 %RH |
| C-ID | all | 1000 ID + EUI reads at 100 and 400 kHz | 0 NACK, 0 mismatch |

### Stage 4: TL-SN-001 power, before the MCU matters

PPK2 source mode on J3 (3.7 V) with the 10 kΩ NTC cable; no USB; ESP held in reset first.

| ID | Test | Pass | Tool |
|---|---|---|---|
| S-P01 | First power | V3V3_CORE 3.25–3.35 V; abort > 3.45 V or > 50 mA | PPK2, A |
| S-P02 | VBAT ramp 2.8→4.0→2.8 V | SYS on between 3.08–3.46 V [VBATPOR], off near 3.0 V | PPK2, A |
| S-P03 | USB only, no pack (IT-10) | CHG_IN ramps ~780 V/s; ST = 1; PG = 0; boots; no resets in 10 min | A |
| S-P04 | PR1 threshold: bench PV 6 V, PPK2 sweeps IN1 5.0→3.5→5.0 V | ST flips at 3.99–4.39 V falling, 4.07–4.43 V rising | A |
| S-P05 | USB priority with PV 6 V and 12 V | ST = 1; PV draws < 50 µA | supply |
| S-P06 | PV only 5/6/7 V | ST = 0; USB_VBUS < 0.5 V | supply |
| S-P07 | Reverse PV −7 V, 100 mA limit; then 12 V and 14 V | ≤ 0.5 mA; D3 conducts only above 13.3 V; boards still work | supply |
| S-P08 | Switchover ×20 with battery + PV (IT-09) | V3V3_CORE ≥ 3.0 V, no brown-out, one ST change per plug | scope |
| S-P09 | Charger ID and reset values; watchdog revert after 170 s | defaults as SLUSF69A; reverts with WATCHDOG 00 | A |
| S-P10 | NTC pigtail 10k/20k/30k/4.3k/2.7k/open with emulated battery | TS_STAT 00/10/01/11/01/open; record whether open TS charges | semi |
| S-P11 | Charge current 40/100/200/300 mA; precharge; termination at 4.2 V | ±10 %; 4.179–4.221 V | A |
| S-P12 | Ship mode and shutdown; wake by VIN | < 5 µA / < 0.6 µA | A |
| S-P13 | Thermal regulation onset | none at ≤ 0.45 W | A |
| S-P14 | Core ripple at 20 µA / 1 mA / 10 mA / 100 mA | ≤ 50 mVpp | scope |
| S-P15 | Start-up waveform; 200 power cycles incl. slow ramps and 50 ms dropouts | ~3.9 ms to regulation, overshoot < 3.45 V, 0 brown-out resets | scope + A |
| S-P16 | Max TX bursts at VBAT 4.2→3.0 V, all SW rails on | core ≥ 3.0 V; 0 brown-outs in 1000 bursts | scope + A |

### Stage 5: TL-SN-001 MCU and root bus

| ID | Test | Pass | A |
|---|---|---|---|
| S-M01 | Enumerates 303A:1001, ×20 replugs | 20/20 | A |
| S-M02 | `esptool chip_id flash_id`; `espefuse summary` (read-only, never burn) | ESP32-H2, 4 MB; JTAG fuses clear | A |
| S-M03 | 4 MB random image write/read SHA | match | A |
| S-M04 | Boot log; SW2+SW1 download mode | POWERON, SPI boot; DOWNLOAD | A + one manual press |
| S-M05 | Root scan; mux mask 0x0–0xF read-back | exactly {0x20, 0x21, 0x6A, 0x70}; equal | A |
| S-M06 | Expanders: POR values; write output then config; read back every pin | Config 0xFFFF, Output 0xFFFF at POR; match | A |
| S-M07 | ESP warm reset with non-default expander outputs | safe state (SW off, mux off) within 50 ms | A |
| S-M08 | PRESENT_PROBE high/low, no cards | PRESENT_N follow; EXP_INT_N falls then clears after reading all ports | A |
| S-M09 | LED step | +1.4–1.9 mA | A |
| S-M10 | VBAT ADC at 3.0/3.6/4.2 V after one-point cal | ±50 mV | A |
| S-M11 | USB D+ backfeed: battery only, USB unplugged, awake and asleep | USB_VBUS < 0.5 V | A |
| S-M12 | 802.15.4 energy scan vs a DevKitM-1 at the same spot | within 6 dB | A |
| S-M13 | OTA slot write/read/erase; image size headroom | SHA match; ≥ 15 % free | A |
| S-M14 | Deep sleep, empty slots, then 4 reference cards (IT-08) | within +20 % of `power_budget.py`; ≤ 40 µA | A |
| S-M15 | Second-master check: TL-DBG-001 on the SN root bus with the ESP held in reset | same scan as S-M05 | A |

### Stage 6: per-slot tests with TL-TEST-001 (H1, run by the node's firmware)

For each slot n, all other mux channels off. Every step reports per contact.

| ID | Test | Pass |
|---|---|---|
| L-01 | Branch n scan; other branches; mask 0 (IT-01) | {0x27, 0x50} only on n; nothing elsewhere |
| L-02 | AON_n / SW_n switching, card TP1/TP3 and SW sense | on within 50 mV of core; off < 0.1 V in 1 ms; other slots unchanged |
| L-03 | SW current step on battery (PPK2) | ΔI ≈ 9.9 mA ±10 % |
| L-04 | Idle: WAKE 0, ALERT 1, PRESENT 0 with probe high | exact |
| L-05 | WAKE drive high/low; deep-sleep wake and latency (IT-02) | only GPIO(10+n) follows; wake mask bit n; < 5 ms |
| L-06 | ALERT low → EXP_B bit and EXP_INT_N (IT-04) | follows within 4 µs; read clears |
| L-07 | MISO and UART_RX driven both ways against internal pulls | follow |
| L-08 | Walking 1/0 on SCK, MOSI, CS_N, RESET_N, UART_TX | card inputs follow |
| L-09 | Cross-direction shorts: drive neighbours opposite | no coupling |
| L-10 | UART mux 4×4 matrix and byte loopback at 115200/1 M (IT-06) | identity matrix; 1 kB PRBS 0 errors, only on n |
| L-11 | Stuck-SDA recovery (needs the MUX_RESET_N decision) | root works with n deselected; recovers after reset |
| L-12 | Backfeed with AON_n off, others active (IT-07) | < 1 µA, AON_n < 0.3 V |
| L-13 | AON off with CS/RESET low, then on | card shows POR values |
| L-14 | CS/RESET/SW_EN at power-up (IT-05) | CS high, RESET high, SW off until firmware |

### Stage 7: end to end

| ID | Test | Pass |
|---|---|---|
| E-01 | Serial bridge to `talos_hub`: boot, heartbeat, each card's events | hub journal shows them with correct device and slot |
| E-02 | ConfigUpdate (IMU threshold): sub- and above-threshold self-test impacts | `VERIFIED` only with the applied revision and the behaviour change; `ACKED` alone is a fail |
| E-03 | OTA over the bridge, with power cut at random points | new version reported after confirm, or clean rollback |
| E-04 | Twin scenario corpus on the real node (L4) | every expectation met or explicitly `skipped` |
| E-05 | 24 h soak, all four slots populated | no resets, no stale data shown as healthy, sleep current stable |

## 5. Fault isolation trees

Each tree runs the next test that separates causes. Example, "module in slot n not discovered":

1. Does the card read on TL-DBG-001? No → card fault (go to its Stage 3 tests).
2. TL-TEST-001 in slot n: does branch n answer at 0x27? Yes → module driver or card.
3. PRESENT_N n low? No → seating or connector.
4. AON_n on (TEST-001 TP1)? No → U6–U9 or the EXP_A AON_EN path.
5. Root bus alive after selecting n? No → branch short (R34–R41, D4–D7, U14 channel).
6. Otherwise → BoardSupport branch or wake mapping (firmware).

Other trees to write: no USB enumeration, wrong core voltage, brown-out on TX, no charging, slot wake missing,
sleep current high (bisect by AON_n off one slot at a time, then by the core split link if fitted).

## 6. Host tests to add

- **Drivers against register fakes:** SHT40 busy-NACK, CRC vector 0xBEEF→0x92, heater duty gate; LIS2DW12 CTRL7/LIR,
  pulsed vs latched, self-test limits, gravity excluded from peak, stuck interrupt; TMAG stale axes, RESULT_STATUS,
  NACK while asleep, POR re-init; EKMB warm-up, glitch filter, mask-window rise, stuck high; BQ25186 config
  sequence, watchdog revert, read-to-clear flags, never WATCHDOG 01/10; TCA9535 output-before-config, all-ports INT
  clear, state kept across MCU reset; TCA9546A deselect, read-back, two-bit collision, stuck branch; TS3A5017 select
  sequencing and S-3 interlock; ID EEPROM v1 decode (blank, corrupt, code 0, unknown revision).
- **Runtime:** dead slot vs empty slot, mux fault, bus stuck, per-slot backoff, brown-out breadcrumb quarantine,
  power-source decode (USB/PV/battery/unknown), reset cause reported, never select a branch whose AON is off.
- **Contracts:** firmware registry = dbg table = `module_code` in every manifest, TL-TEST-001 included; TPS63900 CFG
  resistors decode to 3.3 V (in `check_kicad.py`); slot rail caps vs the 400 mA regulator.
- **tools/dbg fake:** line levels, slow AON decay, TCA9535 on the card, UART loopback, foldback (short = USB
  disconnect), GP1 interrupt flag, EEPROM page writes with ACK polling.

## 7. Software to build

| Area | Work |
|---|---|
| Portable firmware | **Done:** `board_io` (TCA9535, BQ25186, rails with S-2/S-3, presence, source), ID block v1 with CRC, driver fixes FW-B1..B11, `PowerSource::kUnknown`, `diag` console (`@T {json}` lines; self-tests for the root bus, the TL-TEST-001 slot loopback, LIS2DW12 self-test and SHT40 heater). **Open:** mux reset line (needs the board decision), SPI/UART loopback lines (need GPIO output in the HAL) |
| ESP-IDF | **Done:** console and diag on USB-serial-JTAG, reset cause in the boot line, brown-out detector, battery ADC, `BoardIo` for source and rails. **Done:** CoAP over the console (`serial_link.h`, provisioned identity in NVS). **Open:** wake cause, coredump, custom partition table, never deep-sleep on USB |
| Virtual board | **Done:** TCA9535 ×3, BQ25186, rail-gated branches, PRESENT/ALERT, busy-NACK, pulsed INT, polled stepping, per-device NACK, blank/corrupt IDs, `bench_node` with `--fault=` (`firmware/sim/sim_faults.h`). **Open:** EXP_INT line, stuck SDA, brown-out |
| Twin | `IDeviceBackend` (L1/L2/L4), `binding` in the site proto, `--backend`, `skipped` reason, L1/L2 parity test; PIR mask/warm-up, humidity, magnet field, ODR-sampled impacts |
| tools/dbg | `selftest --stage adapter|tl-test-001`, `id program` (refuses overwrite without `--expect-current`), `id --raw`, `reg`, `uart`, `watch` on the interrupt flag, AON cycle waits for SDA low, FW-B13 |
| tools/bench, tools/hil | orchestrator (instruments, `plan.yaml`, JSONL, fault trees); `serial_bridge.py` to the hub; PPK2 wrapper |

## 8. Board changes

### Recommended, no owner decision needed (passives already stocked, copper, silk)

- **TL-SN-001**
  - R_SYS and R_CORE: a 0R with a DNP 2-pin header beside it, in two places. One splits charger from regulator,
    the other splits regulator from its loads, so each block can be powered and metered alone.
  - Test pads on CHG_IN, PV_IN, PMUX_ST, BAT_NTC, CHG_INT_N, CHG_PG_N, EXP_INT_N, MUX_RESET_N, USB D+/D−,
    V3V3_CORE at C12, plus 2–3 more GND pads.
  - 0R on USB D+/D− with DNP shunt-C pads (Espressif's recommendation).
  - 0R between J1 VBUS and IN1.
  - DNP CT pads on all TPS22917s.
  - 100k pull-down on EXPB_SPARE, 1M pull-down on PRESENT_PROBE.
  - DNP 1×6 UART0 header; silk labels; fiducials.
- **TL-TEST-001**
  - 10k TX→RX and MOSI→MISO loopbacks.
  - SW sense into a spare TCA9535 input.
  - Pull-downs on SCK/MOSI/UART_TX (100k) and CS_N/RESET_N (1M).
  - 10k on the unused expander pins; SDA/SCL/ALERT pads.
- **TL-DBG-001**
  - QOD 0R on the AON switch.
  - Move the LVC1G125 supply ahead of JP1.
  - 100k pull-downs on SCK/MOSI.
  - Test pads on BRIDGE_TX and USB_VBUS.
- **Cards**
  - Test pad on WAKE (PIR, IMU) and on MAG_INT_N (Hall).
  - Silk V/O/G at the EKMB.
  - Pull both pours back ~1 mm around the SHT40 [DS §5.3].
- **All boards:** board name, revision and serial box on silk; test pads on a 2.54 mm grid.

### Owner decisions

| Decision | Options | Recommendation |
|---|---|---|
| Stuck-bus recovery: MUX_RESET_N has no driver | GPIO23 (U0RXD pad) with a 0R; or GPIO8 with the LED moved to EXPB_SPARE; or accept power-pull recovery | GPIO23 |
| AON default on (100k pull-ups) | keep; or pull-downs and staggered firmware enable | default off: a shorted card no longer boot-loops the node, and an off slot stops costing 33 µA |
| Slot short protection (TPS22917 has no current limit) | accept; or current-limited switch with FAULT | current-limited on SW at least |
| TPS2121 PR1 divider | keep 100k/33k; or 27k/10k (new part) | 27k/10k: hot leakage can fake a USB input with 133 kΩ |
| BQ25186 C10 1 µF 0402 | keep; 2.2 µF (new part) | 2.2 µF: 1 µF 0402 is ~0.5 µF at 4.2 V |
| Reverse-pack protection, 4-pin pack with separate NTC return | add / not | add the P-FET; 4-pin pack when the pack is chosen |
| Slot UART lines | add 100k pull-ups, series 330 Ω, ESD | pull-ups and series R |
| UART validator vs four TL-TEST-001 | test-fixture exemption; or time-shared UART | exemption |
| TL-TEST-001 at-speed loopback | 10k resistors; or LVC1G125 buffers | resistors |
| SHT40 heater power on a slot | ≤ 20 mW only; or allow 110 mW (33 mA) | ≤ 20 mW in the field, 110 mW on TL-DBG-001 only |
| LIS2DW12 sleep ODR | 12.5 Hz (1 µA) or 100 Hz (5 µA) | decide after strike data (C3, Stage 3) |
| PV panel Voc | ≤ 12 V or wider | must be ≤ 12 V with SMAJ12A |
| Module codes | point existing codes at Rev A manifests; or new codes | new codes per Rev A card plus one for TL-TEST-001 |

## 9. Open bench questions

- LIS2DW12: is CTRL7 bit 5 needed for INT1 wake-up? Does a self-test step trip wake-up?
- TMAG5273: are disabled-channel results kept? What clears the latch in wake-and-sleep?
- TPS22917 ramp vs TMAG's 3 V/ms minimum; AON decay time without QOD.
- BQ25186: datasheet default conflicts (VINDPM, IBAT_OCP, device ID); does an open TS still charge?
- Does the TCA9535 catch the BQ25186's 128 µs /INT pulse?
- Size of the USB D+ → VBUS backfeed on battery.
- ESP32-H2 ext1 per-pin wake levels (HD-004).
- Whether the HSEC8 socket mates odd fingers to odd pads given the mirrored footprint (HD-002, sample socket).
