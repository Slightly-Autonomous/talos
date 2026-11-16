# KiCad check results (generated)

`hardware/scripts/check_kicad.py`, kicad-cli **10.0.8**, 2026-11-16. This shows the schematics are self-consistent. **It is not an electrical design review.**

| Board | Parts | ERC errors | ERC warnings | Waived | Netlist round-trip | Safe defaults |
|---|---:|---:|---:|---:|---|---|
| TL-SN-001 | 134 | 0 | 0 | 0 | match | ok |
| TL-MOD-001 | 7 | 0 | 0 | 0 | match | ok |
| TL-MOD-002 | 7 | 0 | 0 | 0 | match | ok |
| TL-MOD-003 | 8 | 0 | 0 | 1 | match | ok |
| TL-MOD-004 | 7 | 0 | 0 | 0 | match | ok |
| TL-TEST-001 | 14 | 0 | 0 | 0 | match | ok |

## Cross-board connector check (sensor_edge_v1)

- All four main-board slots share one pinout; every card's used contacts match it, reserved contacts (5, 19) are unused, and PRESENT_N (18) is grounded on all five cards.

## TL-SN-001

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- Footprints still to draw (H4):
  - J4 (sensor_edge_v1 slot 0): Samtec MEC8-110-02-L-D-EM2 candidate; footprint TBD (HD-002)
  - J5 (sensor_edge_v1 slot 1): Samtec MEC8-110-02-L-D-EM2 candidate; footprint TBD (HD-002)
  - J6 (sensor_edge_v1 slot 2): Samtec MEC8-110-02-L-D-EM2 candidate; footprint TBD (HD-002)
  - J7 (sensor_edge_v1 slot 3): Samtec MEC8-110-02-L-D-EM2 candidate; footprint TBD (HD-002)
  - U1 (ESP32-H2-MINI-1-H4S): footprint TBD from datasheet §11 land pattern; antenna keepout
  - U4 (BQ25186DLHR): footprint TBD: DLH WSON-10 2.2x2.0 (SLUSF69 §12)

## TL-MOD-001

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- Footprints still to draw (H4):
  - J1 (sensor_edge_v1 card fingers): finger footprint TBD (H4): 1.60 mm card, 0.80 mm pitch, mates MEC8-EM2

## TL-MOD-002

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- Footprints still to draw (H4):
  - J1 (sensor_edge_v1 card fingers): finger footprint TBD (H4): 1.60 mm card, 0.80 mm pitch, mates MEC8-EM2
  - U2 (EKMB1101111): TO-5 3-pin, footprint TBD from Panasonic drawing (PCD 5.08 mm)

## TL-MOD-003

  - waived `pin_to_pin`: Pins of type Bidirectional and Power output are connected (Symbol U2 Pin 3 [SDO/SA0, Bidirectional, Line]; Symbol #FLG02 Pin 1 [Power output, Line]). Justification: LIS2DW12 SDO/SA0 is strapped to V3V3_AON to select I2C address 0x19; in I2C mode it is an address input, not a driver (DS11811 §6). Its internal pull-up cannot be disabled, so strapping low would waste current.
- Netlist: KiCad export equals the intended netlist
- Footprints still to draw (H4):
  - J1 (sensor_edge_v1 card fingers): finger footprint TBD (H4): 1.60 mm card, 0.80 mm pitch, mates MEC8-EM2

## TL-MOD-004

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- Footprints still to draw (H4):
  - J1 (sensor_edge_v1 card fingers): finger footprint TBD (H4): 1.60 mm card, 0.80 mm pitch, mates MEC8-EM2

## TL-TEST-001

- ERC: no violations
- Netlist: KiCad export equals the intended netlist
- Footprints still to draw (H4):
  - J1 (sensor_edge_v1 card fingers): finger footprint TBD (H4): 1.60 mm card, 0.80 mm pitch, mates MEC8-EM2

**Overall: PASS**
