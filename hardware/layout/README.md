# H4 layout (Rev A, first pass, NOT reviewed)

The owner chose to start layout before an EE review. Treat everything here as a **first-pass, autorouted prototype layout**. It shows the design can be placed and routed within fab limits, and it gives a manufacturer something concrete to quote and DFM-check. **It is not ready to order.**

## How the boards are made

| Step | Tool | Output |
|---|---|---|
| Schematic + netlist | `scripts/gen_schematics.py` | `kicad/<board>/<board>.kicad_sch`, project rules, Talos symbol and footprint libraries |
| Placement | `scripts/gen_pcbs.py` (KiCad's Python) | Outline, stackup, nets, footprints linked to symbols, pours, keepouts, custom rules (`.kicad_dru`) |
| Routing | `scripts/route_pcbs.py` + Freerouting 2.5.0 | Routed `.kicad_pcb`. Each attempt is DRC-gated, and a board is saved only after zero DRC errors |
| Checks | `scripts/check_kicad.py` | ERC, netlist round-trip, cross-board, safe defaults, **PCB DRC with schematic parity** |
| Fab package | `scripts/export_fab.py` | `fab/<board>/`: Gerbers + drill (zip), pick-and-place CSV, BOM CSV, top/bottom renders |

Reproduce:

```bash
KPY=~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3
python3 hardware/scripts/gen_schematics.py
$KPY hardware/scripts/gen_pcbs.py
FREEROUTING_APP=~/.cache/talos-tools/Freerouting.app/Contents/MacOS/freerouting $KPY hardware/scripts/route_pcbs.py
python3 hardware/scripts/gen_schematics.py   # re-normalise project files after pcbnew saves
python3 hardware/scripts/check_kicad.py --pdf
python3 hardware/scripts/export_fab.py
```

Freerouting is not committed. It is the GPL-3.0 macOS build: Freerouting 2.5.0, which bundles its own Java runtime. You can also set `FREEROUTING_JAR` and `JAVA` to use the plain jar.

## Board summary

| Board | Size | Layers | Notes |
|---|---|---|---|
| TL-SN-001 | 100 × 80 mm | 4: F signal / In1 solid GND plane / In2 signal + GND pour / B signal | **Placed, partially routed.** Freerouting leaves ~147 connections unrouted even after the board grew from 84 × 66 to 100 × 80 mm. Placement and the routed portion pass DRC; only unconnected items remain (`kicad/layout-status.json`). Module at the top edge with the antenna strip clear. Two slots per side edge. USB-C, PV and battery along the bottom |
| TL-MOD-001..004 | 16 × 26 mm (PIR card 18 × 30) | 2 | Gold fingers on the bottom edge, stopping 0.15 mm short of the bevel. Tracks only in the finger zone (no pour, no vias) |
| TL-TEST-001 | 22 × 32 mm | 2 | Same finger zone |

Design rules (project file):
- 0.15 mm clearance
- 0.15 mm default track, 0.1 mm minimum at fine-pitch pads
- 0.4 mm `Power` netclass for GND, 3V3 rails, SYS_PATH, VBAT, USB/PV inputs and the regulator switch nodes
- 0.6/0.3 mm vias
- 0.3 mm copper-to-edge, except the deliberate edge connectors (custom rule), backed by a 0.35 mm track/via keepout ring
- copper pours drop unconnected islands

These sit inside typical 2- and 4-layer prototype-service limits. Confirm them against the chosen fab's capability sheet.

## Finishing the Sensor Node layout
A scripted autorouter is not enough for this board (dense edge connectors, radio keepout, power section). Options, in order of preference:
1. A layout engineer finishes it interactively in KiCad, starting from this placement. That review is needed for an order anyway.
2. An AI/cloud router (Quilter, DeepPCB) imports the KiCad project. This needs an account and uploading the design, so owner approval first.
3. Iterate on placement here: hand-tuned positions for the slot-side passives, then autoroute again.

No fab package is exported for TL-SN-001 until routing is complete.

## What a reviewer must check before any order

1. **The MEC8-EM2 socket footprint is a placeholder.** Pad size, setback and host-board thickness are assumed (HD-002). Do not order the Sensor Node until it is replaced with Samtec's drawing.
2. **Check the EKMB pin mapping** (Vdd/OUT/GND on a 5.08 mm PCD) against a physical part.
3. **RF:** the module sits on the top edge with Espressif's keepout honoured, but nobody has reviewed antenna ground clearance, enclosure or detuning.
4. **Power layout:**
   - TPS63900: loop and inductor placement were not done to TI's layout guidance.
   - BQ25186: thermal pad vias are not added.
   - TPS2121: still has TBD component values (HD-007).
5. **The autorouter is not a designer.** There is:
   - no impedance control on USB D+/D−
   - no deliberate return paths
   - no length or matching constraints
   - no via stitching beyond the pours
6. **Card fingers:** hard gold vs ENIG, bevel angle and card-thickness tolerance must match the socket datasheet.
7. **Assembly:** everything is on one side except the bottom-layer finger pads. Check DFM with the assembler: 0402 parts, the 0.4 mm-pitch WSON, LGA-12 and DFN-4.
