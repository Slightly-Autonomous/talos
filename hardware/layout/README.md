# H4 layout (Rev A, first pass, NOT reviewed)

The owner chose to start layout before an EE review. Treat everything here as a **first-pass, autorouted prototype layout**. It shows the design can be placed and routed within fab limits, and it gives a manufacturer something concrete to quote and DFM-check. **It is not ready to order.**

## How the boards are made

| Step | Tool | Output |
|---|---|---|
| Schematic + netlist | `scripts/gen_schematics.py` | `kicad/<board>/<board>.kicad_sch`, project rules, Talos symbol and footprint libraries |
| Placement | `scripts/gen_pcbs.py` (KiCad's Python) | Outline, stackup, nets, footprints linked to symbols, pours, keepouts, custom rules (`.kicad_dru`) |
| Functional blocks | `scripts/layout_groups.py` | `kicad/TL-SN-001/TL-SN-001.groups.json`: which parts form each block (a slot, the regulator, the charger, the MCU, the bus), derived from the netlist. KiCadRoutingTools' groups format |
| Placement optimisation | `scripts/place_opt.py` (called by `gen_pcbs.py`) | Simulated annealing (seeded and deterministic, including across Python hash seeds) over every part except the connectors and the radio module. Minimises Manhattan MST wirelength, decoupling-cap distance to its IC pin, RUDY routing congestion and **block spread** (each block pulled to its centroid, plus whole-block moves). It then **lays out slot 0 once and copies it to the other three slots** (the right-hand pair rotated 180°), re-settles the rest around them, and legalises with courtyard halos. KiCad has no auto-placer |
| Hand tweaks | `scripts/layout_tweaks.py` | Lock parts you moved in KiCad, run `capture`, and `gen_pcbs.py` pins them from then on (`<board>.placement.json`) |
| Plane fanout | `scripts/route_pcbs.py` | Before autorouting, every SMD pad on a plane net (GND→In1, V3V3_CORE→In2) gets a clearance-checked stub and via beside it (in the pad only for exposed thermal pads), so the router only handles signals. After routing, GND stitching vias tie the outer pours to In1 |
| Routing | `scripts/route_pcbs.py` + Freerouting 2.5.0 | Routed `.kicad_pcb`. Attempts run in parallel (`ROUTE_JOBS`, default a quarter of the cores), each in its own temp copy of the board folder, and each is DRC-gated. Pins the planes already reach are hidden from Freerouting (it doesn't credit plane connections and otherwise spends ~40 min chasing them). The first clean attempt wins and the rest are stopped. If none is clean, a repair pass keeps the best attempt, rips up tracks in clearance violations and closes leftover open connections: short straight/L tracks first, then a grid maze router (`maze_route.py`, A* on both outer layers with vias, then rip-up of other nets' tracks) |
| Silkscreen | `scripts/silk_pass.py` (run by `route_pcbs.py`) | Every reference designator moved to a spot clear of pads, other silk and the edge; modules carry theirs on the body; a part with no room keeps its reference on the fab layer only. Back-side text is mirrored |
| Block map | `scripts/layout_view.py` | PNG of courtyards coloured by block (with `--tracks`, the routing): the picture used to judge structure |
| Checks | `scripts/check_kicad.py` | ERC, netlist round-trip, cross-board, safe defaults, **PCB DRC with schematic parity**, and a **warning budget**: each DRC warning type must be listed with a count and a reason in `kicad/layout-status.json`, so layout quality can't silently regress. CI checks committed layouts and never routes |
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
| TL-SN-001 | **80 × 70 mm** (was 100 × 80 until 2026-11-20, commit 6f28369) | 4: F signal / In1 solid GND plane / In2 solid V3V3_CORE plane / B signal | **Routed, 0 DRC errors, no hand fixes.** Block-aware annealed placement with the four slots laid out identically, two-pad parts with symmetric courtyards turned to face their nets (pin headers never flipped), three board fiducials, DNP PPK2 split headers JP1/JP2, plane fanout except fine-pitch plane pins next to a power pin, Freerouting. Four HSEC8 sockets with locating slots cut into the side edges. Only silkscreen warnings remain. Module at the top edge with the antenna strip clear. Two slots per side edge. USB-C, PV and battery along the bottom |
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

## Layout evaluation (2026-11-20)

The first layout was compact enough to route but looked scattered: the placer minimised wirelength with no idea of which parts belong together. The overhaul, and what each step bought:

| Step | Result |
|---|---|
| Router benchmark on one placement: Freerouting vs KiCadRoutingTools' autorouter | Freerouting: 0 open, 470 vias, 5654 mm. KRT: 7 open, 694 vias, 5879 mm. **Freerouting kept** |
| Placer benchmark: ours vs KRT `place_simple` | KRT groups by function but leaves loose clusters and moved the fiducials; ours was compact but unstructured. **Ours kept, given blocks** |
| Block cohesion, whole-block moves, slot template copy | Blocks stay together; all four slot circuits are identical copies |
| Outline sweep (100×80, 90×75, 80×70, 75×65) | 75×65 has no room for the debug header. 80×70 places and routes clean once the two sockets per edge keep their 24 mm pitch (their courtyards are 23.3 mm long) |
| Hide plane-connected pins from Freerouting | Same incremental job: 42 → 16 min |
| Silkscreen pass | DRC warnings 100 → 6 (all six are deliberate edge-straddling footprints) |
| Layout red team fixes | Fanout puts vias only in thermal pads; two-pad passives get thermal relief (tombstoning); GND via stitching on a 3 mm grid plus a rescue for pour islands; the maze router keeps hole spacing, reuses same-net vias and tidies rip-up stubs |

**TL-SN-001 at 80 × 70 mm vs the 100 × 80 mm first layout:**

| | 100 × 80 (old) | 80 × 70 (now) |
|---|---:|---:|
| Area | 8000 mm² | 5600 mm² (−30 %) |
| Track length | 5654 mm | 5279 mm (−7 %) |
| Vias | 470 | 488, including ~120 ground-stitching vias |
| DRC errors | 0 | 0 |
| DRC warnings | 100 (silkscreen) | 6 (sockets and module straddling the edge by design; budgeted in `layout-status.json`) |
| Routing time | ~32 min (6 attempts, 3 at a time) | ~37 min (4 attempts at once, plane pins hidden) |
| Hottest part (kisi, 25 °C) | U4 58 °C | U4 61 °C |
| Vias in ordinary SMD pads | ~20 | 1 (Freerouting's; thermal-pad vias are deliberate, HD-039) |

**What is still not possible automatically, and why.**
- **Hand-quality power layout.** No open-source placer knows that the TPS63900's input cap, inductor and output cap form a hot loop to keep tiny, or that the BQ25186 needs thermal vias. Next: per-block templates from each vendor's layout example (fixed relative positions inside the block, placed as one rigid macro).
- **Every reference on silk.** At 0.8 mm text, about a third of the densely packed passives have no clear spot, so their references stay on the fab layer (the assembly drawing). Smaller text would fail the fab's minimum text height.
- **Routing time.** Freerouting is still 30–90 min an attempt on this board and not deterministic, so routing stays local: in parallel, never in CI.
- **Vendor-quality power blocks.** The red team measured the TPS63900 and charger blocks as loose (HD-036..038). Block templates are the next generator step; until then hand-place those blocks and capture them with `layout_tweaks.py`.
- **The last connection or two.** When Freerouting leaves one, the maze router with rip-up closes most of them. Two nets fighting over one channel can still be left open; another Freerouting attempt usually fixes that.

## Finishing the Sensor Node layout
Reproduce: `gen_pcbs.py` (placement), then `route_pcbs.py TL-SN-001` (fanout + parallel Freerouting attempts + repair, about 60–70 min on a 12-core Mac with four attempts in parallel), then `gen_schematics.py` again (a pcbnew save rewrites the `.kicad_pro`, and CI checks generated files are current).

Two placement and fanout rules replaced the hand edits that `finish_sn.py` used to make:
- After annealing, `gen_pcbs.py` turns each two-pad part 180° (only where its courtyard stays put, so pin headers are never turned) when that brings its pads nearer their own nets. Wirelength can't tell the two orientations apart, but L1 the wrong way round forced LX2 to cross LX1.
- `route_pcbs.py` gives no plane via to a fine-pitch plane pin that sits next to a power pin on the same IC (U4 pin 4, U5 pins 2/6/8, U3 pins 3/12). Those vias blocked the switch node and VBAT escapes. The router ties those pins to the exposed pad or a nearby via instead.

Autorouted is not reviewed. A layout engineer still has to review the power section (switch-node loop area, charger thermal pad vias, current-path widths), the radio keepout and the return paths before any order.

## What a reviewer must check before any order

1. **HSEC8-EM2 socket.** The footprint, the host locating slot and the card tongue follow Samtec's drawings (HD-002). Two values were read from drawing geometry, not printed callouts: the slot depth (4.375 mm) and the 0.80 mm pad setback. Check both against a sample socket before ordering. Host and card boards must be 1.57 mm (1.6 mm nominal is within ±0.15 mm).
2. **Check the EKMB pin mapping** (Vdd/OUT/GND on a 5.08 mm PCD) against a physical part.
3. **RF:** the module sits on the top edge with Espressif's keepout honoured, but nobody has reviewed antenna ground clearance, enclosure or detuning.
4. **Power layout** (measured by the 2026-11-20 layout red team):
   - TPS63900: input/output caps 2.8–3.2 mm from their pins, LX traces 8–10 mm with vias, L1 8 mm away (HD-036). Not to TI's layout guidance.
   - BQ25186: one thermal via; no local input cap (HD-037, HD-041).
   - Power/Rail nets neck to ~0.19 mm in places (HD-038).
   - Thermal-pad vias are open vias in pads: order filled/capped (HD-039).
5. **The autorouter is not a designer.** There is:
   - no impedance control on USB D+/D−
   - no deliberate return paths
   - no length or matching constraints
   - no via stitching beyond the pours
6. **Card fingers:** Samtec asks for 30 µin hard gold (130–200 Knoop) and a 30° edge chamfer. Order the cards with JLCPCB's gold-finger option and bevel, not ENIG.
7. **JLCPCB order:** run `jlc_sourcing.py refresh` and `check` the day you order, since stock moves. Upload `-jlc-bom.csv` and `-jlc-cpl.csv`. **Check every part's rotation in JLCPCB's placement preview** (no rotation corrections have been verified yet; record any you find in `ROTATION_FIX`). Parts in `-not-assembled.csv` are not placed by JLCPCB.
8. **Assembly:** everything is on one side except the bottom-layer finger pads. Check DFM with the assembler: 0402 parts, the 0.4 mm-pitch WSON, LGA-12 and DFN-4.

## Checkpoint 2026-11-21: board practices applied, then a change of direction

- **Additions:** lamps, a third expander, tamper loop, test pads, Tag-Connect, mounting holes, regulator
  and charger templates, 0.65 mm vias with 0.5 mm hole-to-hole, and the HD-025..041 fixes. TL-SN-001
  grew to **84 × 72 mm**, because 80 × 70 no longer routed.
- **TL-SN-001:** routing is **partial**, with 2 connections open after Freerouting, the new
  completion pass and maze repair. Two layout rules are waived with reasons in `layout-status.json`.
- **The six small boards:** DRC-clean, and they pass `layout_rules.py`.
- **Router changes:**
  - a Freerouting completion pass before repair;
  - every finishing step keeps the better board (repair rounds that built on each other went from 8 to
    110 errors);
  - stale-reference crash fixes;
  - stitching keeps clear of large holes.
- **Direction change:** the owner then chose to replace the modular node with a self-contained,
  fully JLCPCB-assembled board (`hardware/sourcing/cost-study.md`,
  `hardware/sourcing/zero-touch-assembly-plan.md`). The modular boards stay in the repo as this
  checkpoint and are not being hand-finished.

## TL-SN-002, the self-contained node (2026-11-21, ADR 0019)

- **Board:** 96 × 66 mm, 2 layers, 117 parts, single-sided SMT plus two JLCPCB through-hole parts
  (2 × AA holder, PIR).
- **Same pipeline as before:** generated schematic, block-aware placer with the TPS63900 rigid template,
  Freerouting with a completion pass, keep-best finishing, stitching, power-trace widening, silk pass.
- **New generator features:**
  - cut-outs and reserved label columns act as placer obstacles;
  - a no-track zone on the bottom layer under the regulator's thermal pad, so its vias always fit;
  - thermal vias for boards without inner planes;
  - a duplicate-reference guard in `gen_schematics.py`. It caught two parts both named R99, which had
    silently dropped the ENV lamp resistor.
- **Layout rules (`layout_rules.py`):**
  - TPS63900 template distances;
  - at least 2 thermal-pad vias;
  - lamp strip on the left edge;
  - SHT40 on the slotted thermal tongue;
  - Hall sensor on the right edge;
  - every label printed.
- **Size history:** 92 × 62 placed at about 84 % courtyard use, with no room for the lamp labels, so it
  grew to 96 × 66. A single CR123A would shrink it to about 70 × 50; the owner kept 2 × AA for the longer
  battery life.
