# KiCad check results (generated)

`hardware/scripts/check_kicad.py`, kicad-cli **10.0.8**, 2026-11-20. This shows the schematics are self-consistent. **It is not an electrical design review.**

| Board | Parts | ERC errors | ERC warnings | Waived | Netlist round-trip | Safe defaults | PCB DRC errors | PCB DRC warnings |
|---|---:|---:|---:|---:|---|---|---:|---:|
| TL-SN-001 | 192 | 0 | 0 | 0 | match | ok | 0 | 6 |
| TL-MOD-001 | 11 | 0 | 0 | 0 | match | ok | 0 | 0 |
| TL-MOD-002 | 8 | 0 | 0 | 0 | match | ok | 0 | 0 |
| TL-MOD-003 | 9 | 0 | 0 | 1 | match | ok | 0 | 0 |
| TL-MOD-004 | 7 | 0 | 0 | 0 | match | ok | 0 | 0 |
| TL-TEST-001 | 31 | 0 | 0 | 0 | match | ok | 0 | 0 |
| TL-DBG-001 | 64 | 0 | 0 | 0 | match | ok | 0 | 1 |

## Cross-board connector check (sensor_edge_v1)

- All four main-board slots and the debug adapter's socket share one pinout; every card's used contacts match it, reserved contacts (5, 19) are unused, and PRESENT_N (18) is grounded on all five cards.

## TL-SN-001

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): silk_edge_clearance 6

## TL-MOD-001

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): none

## TL-MOD-002

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): none

## TL-MOD-003

  - waived `pin_to_pin`: Pins of type Bidirectional and Power output are connected (Symbol U2 Pin 3 [SDO/SA0, Bidirectional, Line]; Symbol #FLG02 Pin 1 [Power output, Line]). Justification: LIS2DW12 SDO/SA0 is strapped to V3V3_AON to select I2C address 0x19; in I2C mode it is an address input, not a driver (DS11811 §6). Its internal pull-up cannot be disabled, so strapping low would waste current.
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): none

## TL-MOD-004

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): none

## TL-TEST-001

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): none

## TL-DBG-001

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): silk_edge_clearance 1

**Overall: PASS**
