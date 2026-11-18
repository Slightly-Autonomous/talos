# H4 layout (Rev A, first pass, NOT reviewed)

The owner chose to start layout before an EE review. Treat everything here as a **first-pass, autorouted prototype layout**. It shows the design can be placed and routed within fab limits, and it gives a manufacturer something concrete to quote and DFM-check. **It is not ready to order.**

## How the boards are made

| Step | Tool | Output |
|---|---|---|
| Schematic + netlist | `scripts/gen_schematics.py` | `kicad/<board>/<board>.kicad_sch`, project rules, Talos symbol and footprint libraries |
| Placement | `scripts/gen_pcbs.py` (KiCad's Python) | Outline, stackup, nets, footprints linked to symbols, pours, keepouts, custom rules (`.kicad_dru`) |
| Placement optimisation | `scripts/place_opt.py` (called by `gen_pcbs.py` for TL-SN-001) | Simulated annealing (seeded, deterministic) over every part except the connectors and the radio module. Minimises Manhattan MST wirelength, decoupling-cap distance to its IC pin, and RUDY routing congestion, then legalises with courtyard halos. KiCad has no auto-placer |
| Plane fanout | `scripts/route_pcbs.py` | Before autorouting, every SMD pad on a plane net (GND→In1, V3V3_CORE→In2) gets a clearance-checked stub and via, sized to the pad, so the router only handles signals |
| Routing | `scripts/route_pcbs.py` + Freerouting 2.5.0 | Routed `.kicad_pcb`. Each attempt is DRC-gated; the best attempt is kept |
| Checks | `scripts/check_kicad.py` | ERC, netlist round-trip, cross-board, safe defaults, **PCB DRC with schematic parity** |
| Fab package | `scripts/export_fab.py` | `fab/<board>/`: Gerbers + drill (zip), pick-and-place CSV, BOM CSV, JLCPCB BOM/CPL (`-jlc-bom.csv`, `-jlc-cpl.csv`), a not-assembled list, top/bottom renders |
| JLCPCB sourcing | `scripts/jlc_sourcing.py` | Maps every part to a stocked LCSC part (`sourcing/jlc-parts.csv`). `refresh` queries JLCPCB, `check` runs offline in CI. Parts JLCPCB can't place are listed with their risk ID |

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
| TL-SN-001 | 100 × 80 mm | 4: F signal / In1 solid GND plane / In2 solid V3V3_CORE plane / B signal | **Routed, 0 DRC errors.** Annealed placement, plane fanout (175 pads), Freerouting (40 passes, best of 3), then two hand fixes at fine-pitch power pins (`scripts/finish_sn.py`). Only silkscreen warnings remain. Module at the top edge with the antenna strip clear. Two slots per side edge. USB-C, PV and battery along the bottom |
| TL-MOD-001..004 | 16 × 26 mm (PIR card 18 × 30) | 2 | Gold fingers on the bottom edge, stopping 0.15 mm short of the bevel. Tracks only in the finger zone (no pour, no vias) |
| TL-TEST-001 | 22 × 32 mm | 2 | Same finger zone |
| TL-DBG-001 | 36 × 32 mm | 2 | **Routed, 0 DRC errors**, no hand fixes. Slot socket straddles the top edge (card plugs in beside it), USB-C on the bottom edge, PPK2 meter headers either side of the socket ([HW-0007](../architecture/decisions/0007-slot-socket-and-debug-adapter.md)) |

Design rules (project file):
- 0.15 mm clearance
- 0.15 mm default track, 0.1 mm minimum at fine-pitch pads
- 0.3 mm `Power` netclass for SYS_PATH, USB/PV inputs, CHG_IN and the regulator switch nodes; 0.25 mm `Rail` for the 3V3 rails and VBAT (VBAT's BQ25186 pin is mid-row at 0.4 mm pitch). **EE check:** confirm these widths against the chosen charge current (~0.8 A at 0.25 mm, 1 oz outer, 10 °C rise, IPC-2221 estimate)
- 0.6/0.3 mm vias
- 0.3 mm copper-to-edge, except the deliberate edge connectors (custom rule), backed by a 0.35 mm track/via keepout ring
- copper pours drop unconnected islands

These sit inside typical 2- and 4-layer prototype-service limits. Confirm them against the chosen fab's capability sheet.

## Finishing the Sensor Node layout
Reproduce: `gen_pcbs.py` (placement), then `route_pcbs.py TL-SN-001` (fanout + Freerouting, keeps the best attempt), then `finish_sn.py`, then `gen_schematics.py` again (a pcbnew save rewrites the `.kicad_pro`, and CI checks generated files are current). The `finish_sn.py` step makes two hand edits Freerouting could not find, because plane vias crowded 0.4–0.5 mm pitch pins:
- U4 (BQ25186): pin 4 GND tied to the exposed pad instead of its own via, CHG_PG_N moved up, VBAT runs straight to C10.
- U5 (TPS63900 buck-boost): pin 6 runs directly to output cap C12 instead of a plane via, pin 8 GND ties to the exposed pad, LX2 runs from pin 7 to L1.

`finish_sn.py` matches exact coordinates. After a reroute it may no longer apply; then make the same edits interactively in KiCad.

Autorouted is not reviewed. A layout engineer still has to review the power section (switch-node loop area, charger thermal pad vias, current-path widths), the radio keepout and the return paths before any order.

## What a reviewer must check before any order

1. **The HSEC8-EM2 socket footprint is a placeholder.** Pad size, setback and host-board thickness are assumed (HD-002). Do not order the Sensor Node or the debug adapter until it is replaced with Samtec's drawing.
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
7. **JLCPCB order:** run `jlc_sourcing.py refresh` and `check` the day you order, since stock moves. Upload `-jlc-bom.csv` and `-jlc-cpl.csv`. **Check every part's rotation in JLCPCB's placement preview** (no rotation corrections have been verified yet; record any you find in `ROTATION_FIX`). Parts in `-not-assembled.csv` are not placed by JLCPCB.
8. **Assembly:** everything is on one side except the bottom-layer finger pads. Check DFM with the assembler: 0402 parts, the 0.4 mm-pitch WSON, LGA-12 and DFN-4.
