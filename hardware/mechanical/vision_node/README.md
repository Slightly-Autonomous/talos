# TL-VN-001 Vision Node housing (ADR 0020)

`housing.py` (CadQuery 2.5, `~/.cache/talos-tools/venv-cad`) builds three parts into `out/`: the body,
the back lid and a wall/eave mount plate, as STEP and STL. `render.py` draws `out/vn-exploded.png`
and `out/vn-front.png` with stand-in parts.

```bash
~/.cache/talos-tools/venv-cad/bin/python hardware/mechanical/vision_node/housing.py --check
```

**Size:** 103 × 80 × 55 mm outside, plus the mount tab.

**Material:** ASA, for sheltered outdoor use under an eave (splash-resistant, no vents).

**Print:** the body with its front face down on the bed. The lid goes flat, and the plate base-down.

## What it holds

| Part | How |
|---|---|
| Camera Module 3 Wide NoIR | Four M2 bosses on the front wall, lens centred in an 18 mm aperture behind a 30 × 2 mm acrylic disc (silicone bead in a 1.2 mm recess) |
| Two 850 nm IR boards | Their own M2 posts, each behind its own disc. A ring around the lens window takes a closed-cell foam strip, so IR can't reach the lens by bouncing off the disc (VB-04) |
| Zero 2 W on the PoE HAT | Four M2.5 heat-set inserts in the floor; a 50–100 mm Zero camera cable to the camera |
| Lid microswitch | Normally-open contact to GPIO17, in a pocket on the floor at the back. A finger on the lid presses its lever, 0.6 mm preload |
| PoE cable | PG9 gland in the floor behind the stack. Leave a drip loop outside |
| Mount | M5 bolt and wing nut through the top tab into the plate's clevis for tilt. Pan comes from the plate's slotted screw holes |

## Thermal

The housing has no vents. `housing.py` prints a lumped estimate: natural convection at 6 W/m²K on
367 cm² of ASA, with an assumed 18 K SoC rise over internal air with the heatsink.

| Case | Estimate |
|---|---|
| Day, streaming, 35 °C ambient | SoC about 64 °C |
| Night, IR on | SoC about 74 °C (nights are usually cooler) |

The Pi's soft limit is 80 °C. This is an estimate, not a measurement. VB-01 measures it in the closed
print. If the SoC throttles, the fallbacks are louvres facing down on the floor, or moving the IR
boards outside the camera's enclosure.

## Before printing ASA

**Every bought-part dimension in `housing.py` is provisional** (vendor drawings or typical values).
Check these against the parts that arrive:
- the Zero's hole pattern and the height of the HAT stack;
- the Camera Module 3 hole pattern, and where the lens sits relative to the board edge;
- the IR boards' size and holes;
- the microswitch body and its lever travel.

Print one test body in PETG first, and check four things:
- the 120° view clears the aperture (no dark corners in the frame);
- the switch actuates with the lid on and releases with it off;
- the gland seals;
- the HAT's RJ45 plug fits with its cable bend.
