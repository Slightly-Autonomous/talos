# KiCad check results (generated)

`hardware/scripts/check_kicad.py`, kicad-cli **10.0.8**, 2026-11-22. This shows the schematics are self-consistent. **It is not an electrical design review.**

| Board | Parts | ERC errors | ERC warnings | Waived | Netlist round-trip | Safe defaults | PCB DRC errors | PCB DRC warnings |
|---|---:|---:|---:|---:|---|---|---:|---:|
| TL-SN-002 | 117 | 0 | 0 | 1 | match | ok | 0 | 3 |
| TL-DBG-001 | 74 | 0 | 0 | 0 | match | ok | 0 | 1 |

## Cross-board connector check (sensor_edge_v1)

- All four main-board slots and the debug adapter's socket share one pinout; every card's used contacts match it, reserved contacts (5, 18) are unused, and PRESENT_N (19, HD-025) is grounded on all five cards.

## TL-SN-002

  - waived `pin_to_pin`: Pins of type Bidirectional and Power output are connected (Symbol U8 Pin 3 [SDO/SA0, Bidirectional, Line]; Symbol #FLG06 Pin 1 [Power output, Line]). Justification: LIS2DW12 SDO/SA0 strapped to V3V3 for I2C address 0x19; an address input in I2C mode (DS11811 6), as on TL-MOD-003.
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): silk_edge_clearance 2, silk_over_copper 1

## TL-DBG-001

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): silk_edge_clearance 1

**Overall: PASS**
