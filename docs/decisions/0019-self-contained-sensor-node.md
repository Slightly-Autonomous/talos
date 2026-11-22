# ADR 0019: Self-contained sensor node (replaces the four-slot modular node)

**Status:** Accepted (owner, 2026-11-21): PIR on every board, 2 × AA lithium, and the TMAG5273 INT keeps its inverter (it doesn't depend on the unverified ext1 per-pin polarity, HD-004). Implemented as TL-SN-002.
**Supersedes, once accepted:** the slot parts of HW-0002 (sensor module interface) and HW-0007
(slot socket and debug adapter), and the slot parts of ADR 0013.
**Inputs:** `hardware/sourcing/cost-study.md`, `hardware/sourcing/zero-touch-assembly-plan.md`,
`hardware/mechanical/deployment-and-enclosure.md`.

## Decision

Each node becomes **one self-contained board, assembled 100 % by JLCPCB**. It holds:
- the radio;
- its sensors, soldered on;
- the power path;
- USB-C;
- debug lamps and buttons.

There are no cards and no slots. The four-slot design is kept in the repository as the
2026-11-21 checkpoint (f097b26), not deleted.

Owner guidance (2026-11-21):
- USB-C stays: power, flashing, console and debug, all through the ESP32-H2's built-in USB
  Serial/JTAG.
- Lots of debug LEDs, and the buttons, stay.
- Cost is an important goal, not the only one.
- Keep the methodology and earlier work.
- Having JLCPCB source the ESP32-H2-MINI-1-H4S from a distributor is approved.

## What stays exactly as it is (the methodology)

- **Hub authority and pipeline:** the hub stays authoritative. Journal and `journal_seq` ordering,
  rules, commands with TTL and verification, policy, maintenance and the tamper alarm path,
  website, twin, CI.
- **Generator-only hardware:** `gen_schematics.py` → `gen_pcbs.py` (block-aware placer, rigid
  templates, layout groups) → `route_pcbs.py` (Freerouting with a completion pass, keep-best
  finishing, stitching, widening) → `silk_pass.py` → `check_kicad.py` + `layout_rules.py` →
  `export_fab.py`. No hand-edited KiCad files. Routing stays local; CI only checks.
- **Rules:**
  - JLCPCB sourcing rule plus `jlc_sourcing.py` (the module gets a recorded exception, HD-023).
  - Provisional-hardware rule: pins and parts live in manifests and BoardSupport tables.
  - Design-risk register and bench test plan.
- **Simulation and checks:** spice benches, power budget, kisi thermal, DRC/ERC warning budgets.
- **Board practices** (board-practices-review.md):
  - lamps dark on battery unless service mode;
  - test pads with a map;
  - Tag-Connect;
  - mounting holes;
  - silk labels;
  - fab notes;
  - filled vias;
  - hole-to-hole 0.5 mm.
- **Tamper:** lid and wall switches in series, debounce that fails open, red-team fixes.
- **Firmware core:**
  - runtime, event queue, CoAP link, OTA, NVS ring, watchdog;
  - deep-sleep resume;
  - board identity (`talos_id`), diag console;
  - blink codes, BOOT-button service mode.
- **Drivers:** the sensor drivers (TMAG5273, LIS2DW12, SHT40, PIR) and their tests.

## Hardware: TL-SN-002 (working name)

| Block | Proposal | Reuses |
|---|---|---|
| Radio | ESP32-H2-MINI-1-H4S, global-sourced by JLCPCB | same footprint, pin ledger and antenna keep-out |
| USB-C | USB-C receptacle + USBLC6 ESD + CC resistors. Data goes to the H2's USB pins: flash, console, JTAG debug, all on one cable. VBUS can power the node. | same parts and footprint as TL-SN-001 |
| Battery | **2 × AA lithium (Li-FeS2, e.g. Energizer L91)** in a JLCPCB-placed holder, primary (never charged). Alkaline AA works too, with a shorter life. | – |
| Regulation | **TPS63900 buck-boost** (1.8–5.5 V in, 3.3 V out, 75 nA IQ). AA pairs run from 3.6 V down to about 1.8 V, and VBUS 5 V works too. Full radio power, no TX cap. | the HD-036 rigid template and its spice/PI work |
| Source switchover | USB present → powered from VBUS; battery isolated by a P-FET. No USB → battery. Reverse-cell protection with the same P-FET trick as Q1. **A primary cell can never be charged.** | AO3401A, existing ideal-diode pattern |
| Why not ER14505 + LDO (cost study design B) | Cheaper by about $2 per node, but needs a TX cap or a ~11 dB range cut, a specialty cell, and new regulator work. AA lithium is sold everywhere, handles −40 °C and 2 A pulses, and keeps the proven TPS63900 block. | – |
| Sensors, always fitted | TMAG5273**A2** (3-axis Hall, magnet-defeat detection) at one board edge, toward the magnet. LIS2DW12 near a mounting screw (rigid coupling). SHT40 on a routed-out thermal tongue away from the TPS63900 and radio. | the same drivers, one A1→A2 range change |
| PIR | PIR223D-S0 (JLCPCB through-hole), powered from a GPIO-switched rail. Fitted on every board or as a second build variant of the same board (decide at order time by quantity). Lens in the housing. | PirDriver, with new warm-up timing |
| Tamper | J-loop (lid + wall in series) on an LP GPIO, 1 MΩ (or 2.2 MΩ) / 10 nF | HD-032 design and B7 bench |
| Debug lamps (8–10) | Hardware: **PWR-USB** (VBUS). Firmware: **STAT** (blink codes), **LINK** (Thread state), **MAG**, **VIB**, **PIR**, **ENV**, **TAMP**, **BATT**, **FLT**. Driven by one TCA9535 (existing driver), all on one edge. Policy unchanged: dark on battery, lit on USB or in service mode. | EXP_C logic, LedPolicy, blink-code table |
| Buttons | **RST** (EN), **BOOT** (GPIO9: download mode at reset, service mode after boot), plus **USER** (commission/pair, send a test report, wake). | same parts and behaviour |
| Debug access | USB-C first. Tag-Connect UART0 pads as recovery. Test pads on every rail, the I²C bus, each interrupt line and VBAT, with the test-pad map exported. | D-01, D-02, D-03, D-05 |
| Board | 2-layer if the module ground and the TPS63900 loop allow it (check in layout), else 4. About 70 × 50 mm. M2.5 holes, fiducials, silk labels, JLC order-number spot, single-sided SMD plus JLCPCB through-hole. | everything in gen_pcbs/silk/fab |
| Gone | Slot sockets, eight rail switches, I²C mux, UART mux, two of three expanders, presence/alert/CS lines, card ID EEPROMs, charger, PV input, power mux (TPS2121), 18650 connector. | – |

**Wake pins:** the ESP32-H2 has 19 GPIOs; LP GPIO8–14 can wake it from deep sleep.
- GPIO8: STAT LED.
- GPIO9: BOOT button.
- GPIO10–14 (five wake lines): Hall INT, accel INT, PIR out, tamper, USER button.
- The TMAG5273 INT is open-drain active-low, so it either keeps its inverter (as on TL-MOD-001) or
  uses ext1 per-pin polarity (HD-004).
- Other assignments: USB on 26/27, I²C on 2/3, VBAT ADC on 1, PIR power enable plus a spare on
  0/4/5, UART0 on 23/24 to the Tag-Connect pads.

## Firmware changes

| Area | Change | Effort |
|---|---|---|
| `hal.h` BoardSupport | Add a **built-in module** option per slot (`SlotWiring.fixed_module`, a module code), plus `mux_address = 0` meaning no mux. A board with fixed modules skips EEPROM discovery and presence probing. Existing fields that are unwired simply switch features off (already how the table works). | small |
| `board_tl_sn_002.h` (new, ESP-IDF) | The pin table above. TL-SN-001's file stays for the archived board. | small |
| `board_io.*` | No mux select, no rail switching per slot. PIR rail through a GPIO. Lamps on the one expander. | medium |
| `runtime.*` | Discovery path: built-in modules are present from boot; no card insert or remove. Quarantine stays for a faulting sensor, which is disabled and reported, not power-cycled per slot unless the PIR rail. `kCardRemoved` never fires on this board. Wake mask built from five direct lines (no EXP_WAKE). | medium |
| `modules.*` | TMAG5273 A2 range (±133/±266 mT instead of ±40/±80; check against the datasheet). PIR223D timing in PirDriver. | small |
| Power | USB-present detection (a VBUS divider into an LP GPIO or ADC). Battery level from AA voltage under load. Power budgets per source in BoardSupport. | small |
| `indicators.*` | More lamp roles (LINK, per-sensor, BATT, FLT) in the existing policy and blink tables. | small |
| Virtual board (`firmware/sim`) | A TL-SN-002 model: built-in sensors, no mux/expanders/presence, PIR rail, AA and USB source switching. All existing tests stay for TL-SN-001; new runs for TL-SN-002. | medium |

## Hub, core, contracts, website, twin

- **Hub, rules, journal, commands, policy, maintenance:** **no change.** The node reports the same
  capabilities (contact/magnet, vibration, environment, motion, tamper.enclosure).
- **Contracts (`contracts/manifests`):** a new board manifest `sensor_node_v2.yaml` with its
  built-in modules declared (fixed slots), power sources (battery, USB), indicators and wake lines,
  all `provisional: true`. Module manifests are reused, plus a PIR223D one. Proto: **append-only**,
  e.g. `SlotSpec.built_in_module` (buf breaking must pass).
- **Validator (`core/manifest`):**
  - Fixed slots are always "populated".
  - Slot-only checks (I²C branches, per-slot rail budgets, `module_slot_limits`) stay for TL-SN-001
    and are skipped where a board has no slots of that kind.
  - Root I²C conflict checks stay.
- **Website:** device pages list built-in sensors instead of slots (small change). Tamper,
  maintenance, health and unknown/stale rules are unchanged (ADR 0011).
- **Twin:** an L2 board model for TL-SN-002. The garage and driveway sites move to it. L1/L2 parity
  and every scenario must still pass. The TL-SN-001 model stays available.
- **devsim:** a node type for TL-SN-002, with the same capabilities.

## Tools and bench

- **tools/bench:** slot-specific steps (card presence, branch isolation, per-slot rails, flipped
  card) stay for TL-SN-001. New TL-SN-002 steps cover:
  - each built-in sensor answering;
  - every lamp;
  - the three buttons;
  - USB/battery switchover;
  - PIR rail;
  - tamper (S-M16/17/19 reused);
  - board identity.
- **Flashing:** one command over USB-C (`talos_dbg program-id` + flash + self-test). JLCPCB can flash
  during assembly later (about $16–25 per batch).
- **tools/dbg (the MCP2221 adapter) and TL-DBG-001 / TL-TEST-001:** they exist to exercise cards.
  They are **archived with the modular checkpoint** and not built for TL-SN-002; USB-C on the node
  replaces them.

## Hardware pipeline and CI

- **Generators:**
  - `gen_schematics.py`: a `sensor_node_v2()` board.
  - `gen_pcbs.py`: a spec with the TPS63900 template reused, an edge position for the Hall, the
    thermal tongue for the SHT40, and the lamp strip.
  - `layout_groups.py`: new blocks.
  - `silk_pass.LABELS`: new labels.
  - `layout_rules.py`: per-board rules — template distances, thermal vias, lamps on the edge, the
    SHT40 tongue and the Hall at the edge.
- **Checks and models:**
  - `check_kicad.py`: card and cross-board checks stay for the archived boards.
  - `jlc_sourcing.py`: new rows plus the module exception.
  - `power_budget.py`: an AA + USB case.
  - `spice_sim.py`: new benches for USB/battery switchover, TX burst from 2 × AA at end of life, the
    PIR rail. Slot benches stay for TL-SN-001.
  - `pi_config.py`: a TL-SN-002 config.
- **CI:** unchanged jobs. The archived boards keep being checked (cheap), and TL-SN-002 is added.

## Docs and requirements

- **This ADR** is accepted or amended.
- **HW-0002/HW-0007:** marked superseded for new nodes.
- **ADR 0013:** amended (built-in modules).
- **`hardware-requirements.yaml`:**
  - retire HWR-002–006 and HWR-013 (slots and cards), and HWR-010/011 (charger);
  - change HWR-008 (sources: battery + USB) and HWR-012 (flash over USB-C);
  - HWR-018 module exception.
- **design-risks.md:**
  - HD items that close: slot items HD-002, 005, 025–028 and 040, and charger items HD-007,
    HD-024, HD-029, HD-037 and HD-041;
  - HD items that stay: 004, 018, 023, 032–036, 039;
  - new item: AA pulse and cold behaviour.
- **Bench plan:** a TL-SN-002 section.
- **`talos-project-overview` memory:** updated.

## Cost (estimates; real figures need a JLCPCB quote)

The cost study's design B′ (TPS63900 + 2 × AA, which has no USB-C or debug lamps) costs about
**$26 / $11 / $8 per node landed at 5 / 50 / 500**. Adding USB-C with ESD, the TCA9535, about 10
lamps with resistors and three buttons adds about $1.5–2.5 per node at volume, and a few dollars of
extended-part fees per order at 5. Estimate:

| Units | Per node (estimate, landed, plus duty) |
|---|---|
| 5 | about $28–32 |
| 50 | about $12–14 |
| 500 | about $9–11 |

The modular design cost about $160–220 per 5-unit set. USB-C, lamps and buttons cost a little more
per board, and save bring-up and field-debug time (owner's call, agreed).

## Open questions for the owner

1. PIR on every board, or a PIR build variant? It's the same PCB; decide per order.
2. 2 × AA lithium (recommended) or the ER14505 single cell from the cost study?
3. Keep the TMAG5273 inverter, or rely on ext1 per-pin polarity? Decided at bench (HD-004).

## Implementation notes (software, 2026-11-21)
The owner accepted this on 2026-11-21 with the PIR on every board, 2 x AA lithium primary cells, and the TMAG INT inverter kept (active-high wake). The status line above is left for the owner to change. Software side, all PROVISIONAL like the board:
- **Firmware:** see ADR 0013 amendment 2026-11-21 (b). Built-in modules as fixed slots; no mux; PIR_PWR on EXP P10; eight lamps; USER button (`EventKind::kUserButton`, appended); USB_PRESENT; AA level from the voltage under load (low 2.2 V pair, from `hardware/results/power-budget-sn2.md`). `firmware/esp_idf/main/board_tl_sn_002.h` is the default ESP-IDF target; TL-SN-001 stays buildable (`sdkconfig.tl_sn_001`, built in CI).
- **TMAG5273A2:** the x2 range variant is +/-133/266 mT, not +/-80/133 as the table above says. The letter is the address variant, so A2 keeps 0x35. Both still need checking against the datasheet.
- **Contracts:** `contracts/manifests/boards/sensor_node_v2.yaml`, new module manifests `tmag5273a2_magnetic` and `pir223d_motion`; the LIS2DW12 and SHT40 reuse `vibration_lis2dw12_v1` and `environment_sht40_v1`. Proto (append-only): `SlotSpec.built_in_module`, `BoardManifest.battery` (`BatterySpec`). Validator: built-in slots are always populated, skip connector, bus, rail and slot-limit checks, share the root bus, and count against the power budget; new issue `built_in_mismatch`.
- **Hub:** no alarm-path change. The device registry records `user.button` diagnostics (test report, commissioning request) and never acts on them. For `sensor_node_v2` devices, health asks for new AA cells instead of a charge and names the built-in sensor instead of "reseat slot N".
- **Twin:** the garage and driveway nodes are TL-SN-002 and run on its L2 model. The driveway camera is now a separate L1 device (`cam_driveway`). New stimuli `user_button` and `battery_set`; new scenarios `garage_user_test_report` and `garage_aa_low_battery`.
- **Website / devsim:** device pages list built-in sensors for TL-SN-002 and slots for modular boards. The dev property's `node-12` is a TL-SN-002, and devsim has an `sn002` node type with a USER button.
- **Tools:** `talos_bench --board TL-SN-002` (`talos_bench/sn002.py`) and `python3 -m talos_bench.flash` (flash + identity + self-test over USB-C). `talos_dbg program-id --node --board TL-SN-002`. tools/dbg and TL-DBG-001 tests keep running for the archived boards.
- **Unverified:** everything electrical; the ESP-IDF build is compile-checked only in CI; deep sleep is not entered; no TX power cap yet.- **Red team (2026-11-21):** fixes in ADR 0013 amendment (b) ("Red team"): LP RAM keeps the loaded battery level and the built-in retry schedule across deep sleep, USER cooldown, tamper polling while open, mode hysteresis, PIR warm-up quarantine, board-id target check. The validator also checks a built-in's wake line and keeps card addresses off the built-ins' root bus. Hub health flags missing AA cells on USB. Open: the externally reachable USB-C port (JTAG and console without opening the lid) is a production blocker in `open-decisions.md`.
- **USB-C port lock-down (owner decision 2026-11-22: the port stays reachable from outside the closed enclosure).** Mitigated in firmware and configuration instead of moving the port; details in ADR 0013 amendment 2026-11-22.
  - *Console read-only while the lid is closed:* `BoardSupport::console_lock = kLidClosed` on TL-SN-002. Status, identity and readouts answer (`id`, `slots`, `power`, `identity`, `led`, `board-id`, `i2c scan`, `selftest i2c`, `exp`); everything that changes state or could blind the node (`provision`, `board-id <...>`, `i2c wr`/`rd`, `rail`, `uart`, `mux <mask>`, `rescan`, `reboot`, `led service`, sensor self-tests, unknown commands) is refused with `locked: open the enclosure`, counted and reported to the hub as a P1 `kConsoleLocked` diagnostic (first at once, then at most one per minute). Opening the lid, which is the P0 tamper alarm, unlocks it.
  - *Lamps lid-gated:* `BoardSupport::lamp_gate = kLidOpen`. STAT, LINK, MAG, VIB, PIR, ENV, TAMP, BATT, FLT light only with the lid open or in service mode, and service mode starts only with the lid open (BOOT is inside). USB power alone lights nothing; the USER test report keeps its event and heartbeat, its lamp test runs only with the lid open. This replaces "lit on USB" above for TL-SN-002 (TL-SN-001 keeps it). The hardware PWR lamp on VBUS stays: an accepted, minimal leak (it only says USB power is present).
  - *Production profile:* `firmware/esp_idf/sdkconfig.production` (Secure Boot v2 RSA-3072, flash encryption release mode, JTAG off, secure download mode, console kept), external signing, compile-checked in CI. Owner provisioning steps (offline key, `espsecure.py` signing, irreversible first-boot eFuses): `docs/hardware/provisioning.md`, the HD-016 resolution path.
  - *Hub, twin, web:* refusals are diagnostics (`console.locked`), never alarms; three or more in 24 h make the device DEGRADED with "USB console access attempts while closed", named under any other status too. Twin scenario `garage_usb_console_attack` (L1 and L2, parity on the diagnostic count): mutating commands with the lid closed are refused and reported, then opening the lid alarms. The website labels the diagnostic and shows DEGRADED as not healthy.
  - *Tools:* the TL-SN-002 bench plan checks the lock with the lid closed (S-M20) and opens the lid (S-M21) before self-tests, the PIR rail, lamps and buttons; `talos_bench.flash` opens the lid before the identity write; `talos_dbg program-id --node` says "open the lid" on a locked node.
