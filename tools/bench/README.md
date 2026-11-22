# tools/bench: the TL-SN-001 bench runner

Runs the node part of [bench-test-plan.md](../../docs/hardware/bench-test-plan.md) through the
node's diag console and writes one JSON line per test, outside the repository
(`~/talos-bench/<timestamp>.jsonl`), ending with a summary line.

```bash
cd tools/bench
python3 -m talos_bench --port /dev/cu.usbmodem1101 --expect 0=TL-TEST-001 1=TL-MOD-003 2=TL-MOD-004
# or, while tools/hil/serial_bridge.py owns the port:
python3 -m talos_bench --console 127.0.0.1:7770 --expect 0=TL-TEST-001
```

`--expect` says which card is in which slot. The runner checks, in order:

| Test | What |
|---|---|
| S-M00 | the node answers; expanders and charger took their safe state at boot |
| S-M16 | board identity in `talos_id` (D-09) is `TL-SN-001` rev `A` with a serial; written by `talos_dbg program-id --node` |
| S-M05 | `selftest i2c`: root bus is exactly mux, three expanders (0x20-0x22) and charger; mux and expander read-back; charger config |
| S-M17 | tamper loop J9 reads closed (fit the lid or a jumper); open names J9 |
| S-M18 | EXP_C unused pins driven low and reading low; slot lamps dark on battery (lit for seated cards on USB); `led service on` lights the seated slots. Looking at the lamps stays manual |
| S-M19 | lid opened during deep sleep wakes the node on EXP_WAKE (GPIO14) and reads open. Virtual board only (`sim deepsleep`); manual on a real board until deep-sleep entry is wired |
| C0-01 | `--flipped SLOT=CARD`: a card seated upside down reads present, trips no rail, does not reset the node. Without the option: manual (TL-DBG-001 steps) |
| D-10 | TL-DBG-001 lamps: manual, with the steps (the MCP2221 can't see its own LEDs) |
| T-11 | TL-TEST-001 SW lamp: `--measure T-11=<mA>` is judged against 10.2-11.8 mA with a fault tree; without it, manual |
| S-P09 | power source and charger status are readable |
| slotN.discovered | the expected card is discovered and enabled |
| slotN.*_selftest | TL-TEST-001 slot loopback, LIS2DW12 self-test, SHT40 heater |

Each result is `pass`, `fail` (with a `diagnosis`) or `blocked` (a prerequisite did not pass:
one fault never buries the report in its consequences). Diagnoses come from fault trees that
probe further, for example a seated card that does not answer:

1. PRESENT_N high: card not seated, or finger 19 open (sensor_edge_v1 moved PRESENT_N from 18 to 19, HD-025).
2. Mux refuses the select: U14.
3. Blank or corrupt ID: reprogram it on TL-DBG-001 with `program-id`.
4. Nothing answers on the branch: AON switch, branch pull-ups, ESD array or mux channel (named by
   reference designator).
5. Sensors answer but the ID EEPROM does not: card fault.

## TL-SN-002 (ADR 0019)

The self-contained node has its sensors built in: no `--expect`, no slot steps.

```bash
python3 -m talos_bench --board TL-SN-002 --port /dev/cu.usbmodem1101
# flash + board identity + self-test in one command over USB-C:
python3 -m talos_bench.flash --port /dev/cu.usbmodem1101 --unit-serial SN2-A-0007 [--no-flash] [--dry-run]
```

`talos_bench.flash` shells out to `esptool` (with `firmware/esp_idf/build/flash_args`) or `idf.py`; with
neither installed the flash step is reported `skipped`. Steps: N-01..N-12, S-M16/17/19
([bench-test-plan.md §10](../../docs/hardware/bench-test-plan.md)). Virtual board:
`bench_node --board=TL-SN-002` (faults: `tmag_missing`, `lis_missing`, `sht_missing`, `pir_stuck`,
`lid_open`, `usb_off`, `user_press`, `boot_press`, `battery_mv=<mV>`, ...).

## Tests

```bash
bazel build //firmware/sim:bench_node
TALOS_BENCH_NODE=$PWD/../../bazel-bin/firmware/sim/bench_node python3 -m unittest discover -s tests -t . -v
```

The tests run the whole plan against `firmware/sim:bench_node` with each named fault from
`firmware/sim/sim_faults.h` injected, and check the diagnosis. `tests/test_contracts.py` holds the
part references in diagnoses to the schematic netlist. CI runs both.

The adapter and card stages (1-3) run with `tools/dbg` (`talos_dbg selftest ...`).
Measurements that need a meter, PPK2 or scope are listed in the plan; they are not automated yet.
