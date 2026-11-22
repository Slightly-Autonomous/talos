# Archive: the four-slot modular sensor node (2026-11-14 to 2026-11-21)

**Frozen. Nothing here is generated, routed, checked in CI or ordered.** The owner replaced this design
with the self-contained TL-SN-002 (ADR 0019) on 2026-11-21 and archived it on 2026-11-22. The debug
adapter **TL-DBG-001 stays active** in `hardware/kicad/` (owner, 2026-11-22): it carries one
sensor_edge_v1 slot and a USB bridge, and stays useful for bench work on future sensor boards.

- **Exact state:** git tag `modular-checkpoint-2026-11` (commit 9dbdbeb). It holds everything that
  depended on these boards before the move. To work on one again:
  `git checkout modular-checkpoint-2026-11 -- hardware/kicad/TL-SN-001`. Or regenerate it from the
  generators, which still contain these boards: `python3 hardware/scripts/gen_schematics.py --archived`,
  then `gen_pcbs.py TL-SN-001`. Both write into `hardware/kicad/`.
- **Contents:**
  - `kicad/` holds the KiCad projects plus a snapshot of the libraries they used.
  - `fab/` holds the Gerbers, BOM/CPL, test-pad maps and FAB-NOTES as last exported.
  - `results/` holds the BOMs, pin-to-net tables, the TL-SN-001 power-integrity report and its power
    budget.

| Board | What it was | State at archive |
|---|---|---|
| TL-SN-001 | Main board: ESP32-H2, 4 × Samtec HSEC8 edge slots, BQ25186 charger + PV input, TPS2121 mux, TPS63900, 18650 on a lead, three TCA9535 expanders, I²C mux, per-slot rail switches. 84 × 72 mm, 4-layer | Routed except 2 connections; U5 thermal vias and two silk labels waived |
| TL-MOD-001 | Magnetic card: TMAG5273A1 + inverter | complete |
| TL-MOD-002 | PIR card: Panasonic EKMB (0 stock at JLCPCB) | complete |
| TL-MOD-003 | Vibration card: LIS2DW12 | complete |
| TL-MOD-004 | Temperature/humidity card: SHT40 | complete |
| TL-TEST-001 | Slot test cartridge (loopbacks, rail load) | complete |

## Why it was dropped

1. **It couldn't be built zero-touch.** The HSEC8 edge socket was out of stock at JLCPCB and straddles
   the board edge with pads on both sides, so it is hand-soldered. The EKMB PIR had no stock across the
   whole series. See `hardware/sourcing/zero-touch-assembly-plan.md`.
2. **Cost.** The cards brought about 5 extra PCBs, gold fingers, sockets, mux and rail switches per
   node: about $160–220 per 5-unit set, against about $28–32 per self-contained node at 5
   (`hardware/sourcing/cost-study.md`, estimates).
3. **Physics beats modularity.** Each sensor wants its own spot: the magnet at the frame edge, the PIR
   facing the room, vibration rigidly coupled, temperature/humidity in free air away from heat. One box
   holding four cards satisfies none of them well
   (`hardware/mechanical/deployment-and-enclosure.md` §2).
4. **Power.** The design reached 32.1 µA in sleep (HWR-009 < 40 µA), but only with eight rail switches
   and three expanders. TL-SN-002 lands at about 31 µA with the PIR always on and no switches.

## Lessons worth keeping (all carried into TL-SN-002 or the tooling)

- **Generator-only hardware works:** schematics, placement, routing, silk, checks and fab exports all
  come from scripts, with no hand edits. Two things made it routable:
  - **rigid templates** for the regulator and charger loops (HD-036/037);
  - **cut-outs and keep-outs as placement obstacles**.
- **Routing:** Freerouting alone stalls on dense 2-layer boards. What worked was a completion pass,
  then keep-best finishing, then stitching and power widening. Repeating repair rounds made things
  worse. A board that is too dense says so with "partial", and the fix was more area (80 × 70 → 84 ×
  72), not more router time.
- **Wake lines:** one active-high polarity across every wake source (ext1 ANY_HIGH). That meant an
  inverter on the TMAG5273's open-drain INT (HD-034); TL-SN-002 keeps it.
- **Card keying:** a flipped card must land its PRESENT_N contact on host GND (HD-025, contact 19).
  That rule is what the debug adapter's socket and `check_kicad.py`'s cross-board check still enforce.
- **Tamper:** an RC loop of 1 MΩ / 10 nF is about 12 ms to logic high (spice B7), with debounce that
  fails open. Carried over unchanged.
- **Lamps:** dark on battery, lit on USB or in service mode, all lamps on one edge. TL-SN-002 goes
  further and gates them on the lid.
- **Power budgeting:** the charger's NTC must touch the cell. Charging stops below 0 °C, so winter
  solar is lost. PV charging puts 0.72 W into U4 (HD-041), so the battery bay goes away from the
  charger. All of this is moot for 2 × AA primary cells, but still applies if a rechargeable node comes
  back.
- **Sourcing:** check JLCPCB stock first. Edge-straddling connectors and the EKMB were the two dead ends.
  A stocked through-hole part costs JLCPCB about $0.016 per joint, so through-hole is fine.

## What still references this design

- **Firmware host tests:** `firmware/esp_idf/main/board_tl_sn_001.h` and the TL-SN-001 virtual board
  in `firmware/sim` stay as test fixtures. They exercise the slot code paths in `firmware/portable`
  (discovery, quarantine, per-slot rails), which a future slotted board would reuse. There is no
  ESP-IDF build target for TL-SN-001 any more.
- **Contracts:** `contracts/manifests/boards/sensor_node_proto_a.yaml` and the card module manifests
  stay. The manifest validator's slot checks still run against them, and the twin and devsim can still
  model a slotted node.
- **Interface:** `hardware/interfaces/sensor_edge_v1.md` stays, because TL-DBG-001 still implements it.
- **Simulation:** `spice_sim.py` keeps the slot benches and `magnet_sim.py` keeps the TL-MOD-001
  section. Both are cheap and record verdicts the archive relies on.
