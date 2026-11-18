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
| TL-SN-001 | 100 × 80 mm | 4: F signal / In1 solid GND plane / In2 solid V3V3_CORE plane / B signal | **Routed, 0 DRC errors, no hand fixes.** Annealed placement, two-pad parts turned to face their nets, plane fanout except fine-pitch plane pins next to a power pin, Freerouting (200 passes). Four HSEC8 sockets with locating slots cut into the side edges. Only silkscreen warnings remain. Module at the top edge with the antenna strip clear. Two slots per side edge. USB-C, PV and battery along the bottom |
| TL-MOD-001..004 | 16 × 26 mm (PIR card 18 × 30) | 2 | **Routed, 0 DRC errors.** A 9.25 mm wide, 8.5 mm deep tongue carries the gold fingers; the outline is cut back either side of it. Fingers stop 0.15 mm short of the bevel. No parts anywhere on the tongue, because it goes into the socket body (`gen_pcbs.py` refuses a placement that puts one there). Tracks only in the 4 mm finger zone (no pour, no vias) |
| TL-TEST-001 | 22 × 32 mm | 2 | **Routed, 0 DRC errors.** Same tongue and finger zone |
| TL-DBG-001 | 40 × 36 mm | 2 | **Routed, 0 DRC errors**, no hand fixes. Slot socket straddles the top edge with its locating slot cut beside pin 1 (card plugs in beside it), USB-C on the bottom edge, PPK2 meter headers either side of the socket ([HW-0007](../architecture/decisions/0007-slot-socket-and-debug-adapter.md)) |

Design rules (project file):
- 0.15 mm clearance
- 0.15 mm default track, 0.1 mm minimum at fine-pitch pads
- 0.3 mm `Power` netclass for SYS_PATH, USB/PV inputs, CHG_IN and the regulator switch nodes; 0.25 mm `Rail` for the 3V3 rails and VBAT (VBAT's BQ25186 pin is mid-row at 0.4 mm pitch). **EE check:** confirm these widths against the chosen charge current (~0.8 A at 0.25 mm, 1 oz outer, 10 °C rise, IPC-2221 estimate)
- 0.6/0.3 mm vias
- 0.3 mm copper-to-edge, except the deliberate edge connectors (custom rule), backed by a 0.35 mm track/via keepout ring
- copper pours drop unconnected islands

These sit inside typical 2- and 4-layer prototype-service limits. Confirm them against the chosen fab's capability sheet.

## Finishing the Sensor Node layout
Reproduce: `gen_pcbs.py` (placement), then `route_pcbs.py TL-SN-001` (fanout + Freerouting, keeps the best attempt), then `gen_schematics.py` again (a pcbnew save rewrites the `.kicad_pro`, and CI checks generated files are current).

Two placement and fanout rules replaced the hand edits that `finish_sn.py` used to make:
- After annealing, `gen_pcbs.py` turns each two-pad part 180° when that brings its pads nearer their own nets. Wirelength can't tell the two orientations apart, but L1 the wrong way round forced LX2 to cross LX1.
- `route_pcbs.py` gives no plane via to a fine-pitch plane pin that sits next to a power pin on the same IC (U4 pin 4, U5 pins 2/6/8, U3 pins 3/12). Those vias blocked the switch node and VBAT escapes. The router ties those pins to the exposed pad or a nearby via instead.

Autorouted is not reviewed. A layout engineer still has to review the power section (switch-node loop area, charger thermal pad vias, current-path widths), the radio keepout and the return paths before any order.

## What a reviewer must check before any order

1. **HSEC8-EM2 socket.** The footprint, the host locating slot and the card tongue follow Samtec's drawings (HD-002). Two values were read from drawing geometry, not printed callouts: the slot depth (4.375 mm) and the 0.80 mm pad setback. Check both against a sample socket before ordering. Host and card boards must be 1.57 mm (1.6 mm nominal is within ±0.15 mm).
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
6. **Card fingers:** Samtec asks for 30 µin hard gold (130–200 Knoop) and a 30° edge chamfer. Order the cards with JLCPCB's gold-finger option and bevel, not ENIG.
7. **JLCPCB order:** run `jlc_sourcing.py refresh` and `check` the day you order, since stock moves. Upload `-jlc-bom.csv` and `-jlc-cpl.csv`. **Check every part's rotation in JLCPCB's placement preview** (no rotation corrections have been verified yet; record any you find in `ROTATION_FIX`). Parts in `-not-assembled.csv` are not placed by JLCPCB.
8. **Assembly:** everything is on one side except the bottom-layer finger pads. Check DFM with the assembler: 0402 parts, the 0.4 mm-pitch WSON, LGA-12 and DFN-4.
