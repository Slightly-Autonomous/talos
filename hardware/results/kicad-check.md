# KiCad check results (generated)

`hardware/scripts/check_kicad.py`, kicad-cli **10.0.8**, 2026-11-21. This shows the schematics are self-consistent. **It is not an electrical design review.**

| Board | Parts | ERC errors | ERC warnings | Waived | Netlist round-trip | Safe defaults | PCB DRC errors | PCB DRC warnings |
|---|---:|---:|---:|---:|---|---|---:|---:|
| TL-SN-001 | 232 | 0 | 0 | 4 | match | ok | 2 | 7 |
| TL-SN-002 | 117 | 0 | 0 | 1 | match | ok | 0 | 3 |
| TL-MOD-001 | 11 | 0 | 0 | 0 | match | ok | 0 | 0 |
| TL-MOD-002 | 8 | 0 | 0 | 0 | match | ok | 0 | 0 |
| TL-MOD-003 | 9 | 0 | 0 | 1 | match | ok | 0 | 0 |
| TL-MOD-004 | 7 | 0 | 0 | 0 | match | ok | 0 | 0 |
| TL-TEST-001 | 33 | 0 | 0 | 0 | match | ok | 0 | 0 |
| TL-DBG-001 | 74 | 0 | 0 | 0 | match | ok | 0 | 1 |

## Cross-board connector check (sensor_edge_v1)

- All four main-board slots and the debug adapter's socket share one pinout; every card's used contacts match it, reserved contacts (5, 18) are unused, and PRESENT_N (19, HD-025) is grounded on all five cards.

## TL-SN-001

  - waived `pin_to_pin`: Pins of type Open collector and Power output are connected (Symbol U6 Pin 5 [QOD, Open collector, Line]; Symbol U6 Pin 6 [VOUT, Power output, Line]). Justification: TPS22917 QOD tied to VOUT on purpose (HD-028): SLVSDW8B 8.3.3 uses QOD to VOUT for the on-chip quick discharge. QOD is the drain of the discharge FET, only on while the switch is off, so it never fights the VOUT driver.
  - waived `pin_to_pin`: Pins of type Open collector and Power output are connected (Symbol U9 Pin 5 [QOD, Open collector, Line]; Symbol U9 Pin 6 [VOUT, Power output, Line]). Justification: TPS22917 QOD tied to VOUT on purpose (HD-028): SLVSDW8B 8.3.3 uses QOD to VOUT for the on-chip quick discharge. QOD is the drain of the discharge FET, only on while the switch is off, so it never fights the VOUT driver.
  - waived `pin_to_pin`: Pins of type Open collector and Power output are connected (Symbol U7 Pin 5 [QOD, Open collector, Line]; Symbol U7 Pin 6 [VOUT, Power output, Line]). Justification: TPS22917 QOD tied to VOUT on purpose (HD-028): SLVSDW8B 8.3.3 uses QOD to VOUT for the on-chip quick discharge. QOD is the drain of the discharge FET, only on while the switch is off, so it never fights the VOUT driver.
  - waived `pin_to_pin`: Pins of type Open collector and Power output are connected (Symbol U8 Pin 5 [QOD, Open collector, Line]; Symbol U8 Pin 6 [VOUT, Power output, Line]). Justification: TPS22917 QOD tied to VOUT on purpose (HD-028): SLVSDW8B 8.3.3 uses QOD to VOUT for the on-chip quick discharge. QOD is the drain of the discharge FET, only on while the switch is off, so it never fights the VOUT driver.
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): unconnected_items 2, silk_edge_clearance 6, silk_over_copper 1
- **Routing incomplete** (2 unconnected items): 84 x 72 mm (2026-11-21). Autorouted after the board-practices additions; 2 connections left open by the router (Freerouting + completion pass + maze repair). Checkpoint only: the owner chose to replace the modular node with a self-contained board (hardware/sourcing/cost-study.md), so these are not being hand-finished.

## TL-SN-002

  - waived `pin_to_pin`: Pins of type Bidirectional and Power output are connected (Symbol U8 Pin 3 [SDO/SA0, Bidirectional, Line]; Symbol #FLG06 Pin 1 [Power output, Line]). Justification: LIS2DW12 SDO/SA0 strapped to V3V3 for I2C address 0x19; an address input in I2C mode (DS11811 6), as on TL-MOD-003.
- Netlist: KiCad export equals the intended netlist
- PCB DRC (with schematic parity): silk_edge_clearance 2, silk_over_copper 1

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
