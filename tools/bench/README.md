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
| S-M05 | `selftest i2c`: root bus is exactly mux, expanders and charger; mux and expander read-back; charger config |
| S-P09 | power source and charger status are readable |
| slotN.discovered | the expected card is discovered and enabled |
| slotN.*_selftest | TL-TEST-001 slot loopback, LIS2DW12 self-test, SHT40 heater |

Each result is `pass`, `fail` (with a `diagnosis`) or `blocked` (a prerequisite did not pass:
one fault never buries the report in its consequences). Diagnoses come from fault trees that
probe further, for example a seated card that does not answer:

1. PRESENT_N high: card not seated, or finger 18 open.
2. Mux refuses the select: U14.
3. Blank or corrupt ID: reprogram it on TL-DBG-001 with `program-id`.
4. Nothing answers on the branch: AON switch, branch pull-ups, ESD array or mux channel (named by
   reference designator).
5. Sensors answer but the ID EEPROM does not: card fault.

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
