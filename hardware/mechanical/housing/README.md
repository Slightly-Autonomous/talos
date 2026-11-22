# TL-SN-002 housing (PROVISIONAL, first print)

Generated from the routed board, so a part that moves on the PCB moves its opening here.

| File | What |
|---|---|
| `housing.py` | CadQuery source: base, lid, battery door, magnet carrier. `--check` runs the fit rules only. |
| `render.py` | Exploded and top renders into `out/` (matplotlib). |
| `TL-SN-002.board.json` | Board geometry from `hardware/scripts/housing_marks.py`: holes, switches, PIR, lamps, Hall, tongue, USB-C, keep-outs. |
| `out/*.step`, `out/*.stl` | Printable parts and CAD exchange (regenerate; don't hand-edit). |
| `../../fab/TL-SN-002/TL-SN-002.step` | The assembled board (B-05), for checking fits in any CAD tool. |

```bash
~/.cache/talos-tools/venv-cad/bin/python hardware/mechanical/housing/housing.py
~/.cache/talos-tools/venv-cad/bin/python hardware/mechanical/housing/render.py
```

The venv is `python3.12 -m venv ~/.cache/talos-tools/venv-cad && pip install cadquery==2.5.2 matplotlib`.
CI does not build the housing; it is a local tool like the router.

## How it mounts (owner 2026-11-22)

- **Door-frame mount**, latch side:
  - The long side runs horizontally across the casing. The **right edge sits flush with the casing
    edge** nearest the door, so the Hall sensor faces the door.
  - The housing may overhang onto the wall, never toward the door.
  - Two countersunk wood screws go through the base floor into the casing, under the board.
- **The magnet carrier** goes on the door face, level with the Hall sensor.
  - It holds a 10 × 3 mm N42 disc.
  - Gap target: 10 mm or less from the sensor with the door shut. Bench C1-06 measures the real
    door's baseline.
  - The firmware learns the closed-door field at every close. A foreign magnet raises
    `tamper.magnet`.
- **Prying the node off** the frame changes its pose. The accelerometer catches it
  (`tamper.mount`), so there is no back switch to fit.

## Parts and print settings (Bambu Lab P2S, 0.4 mm nozzle)

| Part | Material | Orientation | Notes |
|---|---|---|---|
| base | PETG (indoor) / ASA (outdoor) | floor down | M2.5 heat-set inserts (OD ~3.9, L 4) in the four board bosses. The lid screws self-tap into the 2.6 mm pilots in the left bay and in the lugs; use M3 inserts if they will be undone often. |
| lid | same | top down | 4 × M3 × 12 screws. The SW4 spring finger prints as part of the lid; it needs no support with the lid printed top down. |
| battery door | PETG | top down | Slides out toward +X with no tool; a detent bump holds it shut. Its spring finger presses SW5. |
| magnet carrier | any | flat | 10 × 3 mm N42 disc, glued. Two #4 screws into the door. |
| gasket | TPU 95A | flat | Print a 1.5 × 1.0 mm ring from the base's channel outline, or use 1.5 mm cord. Splash-resistant only; **no IP rating claimed**. |
| light pipes | 1.75 mm clear PETG filament offcuts | — | Cut to lid height (about 19 mm), push into the 1.9 mm holes over the lamps. The lamps only light with the lid open or in service mode, so the pipes are for bench and service use. |

General settings: 0.2 mm layers, 4 walls, 25 % gyroid infill, brim on ASA. **PLA is indoor-only.** It
creeps at 55–60 °C, and creep also slowly relaxes the tamper fingers' preload (HD-049).

## PIR lens (designer's choice, owner 2026-11-22)

Printed plastics block 5–14 µm IR, so the PIR needs a bought **HDPE Fresnel lens**. Requirements:
- 15 mm flange diameter, sitting on the lid's 15.4 mm × 1 mm ledge;
- a 12–13 mm active window;
- multi-zone, wide angle (about 100° or more);
- a focal distance that suits a 9 mm TO-5 PIR, with the element about 5–8 mm below the lens;
- clear or white HDPE, UV-stabilised for outdoor nodes.

Generic "PIR Fresnel lens, 15 mm" parts from several sellers fit. Record the one that passes the bench
test (range and false triggers, HD-044) in `hardware/bom/`. It is not a JLCPCB part: the lens belongs to
the housing, not the board.

## Fit rules `housing.py` checks

- Every part fits the P2S bed.
- The wall in front of the Hall sensor is 1.5 mm or less.
- No lid screw is within 15 mm of the antenna zone (H-03), and none lands on the board.

The spring fingers sit 0.6 mm below each switch's free height (TS-1187A travel is about 0.25 mm), so
the cantilever keeps the switch pressed across normal print tolerance.

## Not verified (first print)

The whole housing is PROVISIONAL. Check on a test print:
- board fit and boss alignment;
- finger preload: the switch closes with the lid or door shut, and opens with 1 mm of lift;
- door slide and detent feel;
- USB-C plug clearance;
- light-pipe alignment;
- PIR lens focus;
- magnet gap on a real door frame.

These are logged under HD-049 and the bench plan.
