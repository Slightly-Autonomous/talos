# KiCad check results (generated)

`hardware/scripts/check_kicad.py`, kicad-cli **10.0.8**, 2026-11-17. This shows the schematics are self-consistent. **It is not an electrical design review.**

| Board | Parts | ERC errors | ERC warnings | Waived | Netlist round-trip | Safe defaults | PCB DRC errors | PCB DRC warnings |
|---|---:|---:|---:|---:|---|---|---:|---:|
| TL-SN-001 | 134 | 0 | 0 | 0 | match | ok | 0 | 104 |
| TL-MOD-001 | 10 | 0 | 0 | 0 | match | ok | 0 | 9 |
| TL-MOD-002 | 7 | 0 | 0 | 0 | match | ok | 0 | 3 |
| TL-MOD-003 | 8 | 0 | 0 | 1 | match | ok | 0 | 16 |
| TL-MOD-004 | 7 | 0 | 0 | 0 | match | ok | 0 | 3 |
| TL-TEST-001 | 14 | 0 | 0 | 0 | match | ok | 0 | 12 |

## Cross-board connector check (sensor_edge_v1)

- All four main-board slots share one pinout; every card's used contacts match it, reserved contacts (5, 19) are unused, and PRESENT_N (18) is grounded on all five cards.

## TL-SN-001

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): silk_edge_clearance 6, silk_over_copper 62, silk_overlap 36
- Footprints still to draw (H4):
  - J4 (sensor_edge_v1 slot 0): placeholder land pattern talos:Samtec_MEC8-110-02-L-D-EM2_PLACEHOLDER
  - J5 (sensor_edge_v1 slot 1): placeholder land pattern talos:Samtec_MEC8-110-02-L-D-EM2_PLACEHOLDER
  - J6 (sensor_edge_v1 slot 2): placeholder land pattern talos:Samtec_MEC8-110-02-L-D-EM2_PLACEHOLDER
  - J7 (sensor_edge_v1 slot 3): placeholder land pattern talos:Samtec_MEC8-110-02-L-D-EM2_PLACEHOLDER

## TL-MOD-001

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): silk_edge_clearance 2, silk_over_copper 5, silk_overlap 2

## TL-MOD-002

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): silk_over_copper 1, silk_overlap 2

## TL-MOD-003

  - waived `pin_to_pin`: Pins of type Bidirectional and Power output are connected (Symbol U2 Pin 3 [SDO/SA0, Bidirectional, Line]; Symbol #FLG02 Pin 1 [Power output, Line]). Justification: LIS2DW12 SDO/SA0 is strapped to V3V3_AON to select I2C address 0x19; in I2C mode it is an address input, not a driver (DS11811 §6). Its internal pull-up cannot be disabled, so strapping low would waste current.
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): silk_over_copper 10, silk_overlap 6

## TL-MOD-004

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): silk_over_copper 3

## TL-TEST-001

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): silk_edge_clearance 1, silk_over_copper 7, silk_overlap 4

**Overall: PASS**
