# Deployment conditions and the 3D-printed housing (first pass, 2026-11-20)

This first pass asks where the boards will actually live and what that demands of the housing and of
the boards. It follows the TMAG5273 placement work, where the Hall sensor went to the card's far edge
to be nearest the door magnet. **Nothing here is applied yet.** Each item is marked:

- **DO**: recommended.
- **OPT**: optional.
- **ASK**: needs the owner's call.

Every dimension is from the current generator output, rounded. Each one must be checked against a
printed test fit.

## 1. What the boards look like physically

- **TL-SN-001 main board:**
  - 84 × 72 mm (grown from 80 × 70 on 2026-11-21 to route the additions), 1.6 mm thick.
  - Four M2.5 corner holes (H1–H4).
  - All parts are on the top side. The bottom carries only the THT pads of USB-C, the socket weld tabs
    and the hole rings.
- **Cards plug in coplanar.** Each card sticks straight out of a side edge, in the board's own plane,
  about 25 mm past the edge (TL-MOD-002 PIR card: about 29 mm). With all four slots filled, the
  assembly is about **136 × 72 mm** and about 12 mm tall above the board. The PIR's TO-5 can is the
  tallest part, plus the USB-C and JST bodies.
- **Bottom edge**, left to right: USB-C, the PV JST, the lamp strip (PWR, STAT, S0–S3) and the battery
  JST.
- **Top edge:** the ESP32-H2 PCB antenna in the middle, with a 5 mm copper-free strip. The tamper JST
  (J9) is at the top-left.
- **Battery:** one protected 18650 on a lead (J3) is about 70 × 19 mm. Because the board's underside
  is free of parts, the cell can sit **under the board** in a stacked housing. The board then sits on
  standoffs above a battery bay, which keeps the footprint at the board's size.

## 2. Where each card wants to be

The four-slot idea meets physics here. Each sensor has its own placement needs, and one box can't
satisfy all four at once.

| Card | What it needs | Consequence for the housing |
|---|---|---|
| TL-MOD-001 magnetic (door/window) | The magnet within roughly 10–20 mm of the TMAG5273, which sits 2.6 mm from the card's far edge. A known geometry for the closed-door baseline (HD-033). | The housing wall over the sensor stays **≤ 1.5 mm**. The node mounts on the frame with that card's edge facing the door, and a moulded mark shows where the magnet goes. A matching printed magnet carrier (a pocket for a 10 × 3 mm N42 disc) keeps the geometry repeatable. |
| TL-MOD-002 PIR | Line of sight through an IR-transparent lens. PLA, PETG and ASA are opaque to 5–14 µm IR. The EKMB looks out normal to the card face, so normal to the board plane. | A **Fresnel lens window** (HDPE, bought, about 10–12 mm) sits directly over the TO-5 can, sealed. The board plane must face the room, so the node mounts flat on a wall. The lens sits off-centre, over that card. |
| TL-MOD-003 vibration (glass/safe/door) | Rigid mechanical coupling to the surface it watches. | A card held only by its socket is a cantilever with its own resonance, and it mutes the signal. The housing needs a **rib or peg clamping the card's far end** and a rigid mount (screws, or thin VHB, never foam tape). |
| TL-MOD-004 temperature/humidity | Free air, and no heat from the board. Charging from solar puts about 0.7 W in U4, and the regulator adds heat. | The card goes in a **separate vented compartment** with a printed wall between it and the main board. Outdoors, the vents get an ePTFE membrane and a sun shade. The SHT40 must never be conformal-coated. Slot rule: the top slots (J4 or J6) are farthest from the charger. |

**Proposal (ASK):** treat housings as per deployment, not per board.
- **Door node:** magnetic + vibration, mounted on the frame.
- **Room node:** PIR + temperature/humidity, mounted on the wall.
- **Outdoor/shed node:** temperature/humidity + vibration, with PV and a sealed housing.

One base design with swappable lids and side caps would keep this to one print family.

## 3. Housing features that matter regardless of deployment

| ID | Item | Rec | Why |
|---|---|---|---|
| H-01 | M2.5 brass heat-set inserts in printed bosses at H1–H4, with **nylon** screws near the top edge | DO | Heat-set inserts are the standard way to put screws into prints. The screws sit 30 mm or more from the antenna, but nylon costs nothing and removes any doubt. |
| H-02 | Card guides: printed slots that support each card's far end and allow insertion only one way | DO | They take load off the socket, which only grips 8.5 mm of tongue. They tame card resonance, which matters for the vibration card. They also add mechanical keying on top of the HD-025 electrical fix. |
| H-03 | Antenna zone: no metal, no carbon-fibre or metal-filled filament, and no screws or inserts within about 15 mm of the module's antenna end. Wall at least 1 mm there | DO | Espressif's guidance for PCB antennas. Carbon-filled filaments are conductive and detune it. |
| H-04 | Material: PLA indoors only. PETG or ASA anywhere warm. ASA outdoors (UV) | DO | PLA softens near 55–60 °C. A dark box in sun reaches 60–70 °C, which is also the Li-ion limit. |
| H-05 | Two tamper switches in series on J9: a lid plunger, plus a **back (wall-removal) switch** pressing through the base onto the wall | DO | This is standard practice for alarm sensors. Prying the node off the wall is as important as opening the lid. Both fit the existing single loop, wired in series, normally closed. |
| H-06 | Light pipe for the lamp strip on the bottom edge (PWR, STAT, S0–S3) | OPT | The firmware keeps the lamps dark on battery. Without a pipe they are visible only with the lid off, which is fine for a security device. |
| H-07 | USB-C opening with a TPU plug, a PV cable gland, and strain relief on the battery lead | DO | Pulled leads are the most common field failure. A gland is needed outdoors. |
| H-08 | Battery bay with NTC contact: the pack NTC against the cell, and the bay away from the charger | DO | The charger's temperature window only protects the cell if the NTC reads the cell. |
| H-09 | Outdoors: IP54-style lid gasket (printed TPU), vents only through ePTFE, drip loop on the PV cable, conformal coat except on connectors, the SHT40, the PIR and the card fingers | DO (outdoor is in scope) | Depends on whether outdoor nodes are in scope for Rev A. |
| H-10 | Access: RST/BOOT, Tag-Connect and test pads reachable with the lid off; no external button | DO | An external button would let someone reset the node without tripping tamper. |

## 4. Possible board changes this suggests (for verdict, not applied)

| ID | Change | Rec | Cost |
|---|---|---|---|
| B-01 | Cards get an M2 hole or a notch near the far end, for the H-02 guide peg | DO for TL-MOD-003, OPT for the others | No parts. It needs card area: the TL-MOD-001 Hall sensor already uses the far edge, so a notch beside it. |
| B-02 | Second tamper input (EXP_C P05 has a spare pin) so lid-open and wall-removal are reported separately | OPT | One JST, one resistor and one capacitor. The series loop in H-05 already works without it. |
| B-03 | Slot assignment rule in the manifest validator: the temperature/humidity card only in slots 0 or 2 (top, farthest from the charger) | DO | Software only. |
| B-04 | Keep-out marks on the fab/courtyard layers for the housing (inserts, card guides, antenna zone) | DO | No parts. A STEP export of the assembled board then goes straight into CAD. |
| B-05 | Export a STEP of each board plus a simple housing reference model (board, cards, cell, keep-outs) | DO | `kicad-cli pcb export step`. It is the starting point for the printed housing. |
| B-06 | Conformal-coating keep-out map on a user layer (connectors, card fingers and sockets, SHT40, PIR, buttons, Tag-Connect, test pads), exported with the fab files | DO (outdoor) | No parts. |

## 5. Owner answers (2026-11-20) and what they change

- **Outdoor is in scope, relatively sheltered** (under eaves or in a porch or shed, no direct rain):
  - H-09 becomes **DO**: a TPU lid gasket and ePTFE-membrane vents, aiming at roughly IP54 (splash
    and dust, not rain). Drip loops on the PV and lead cables. Cable entries face down.
  - **Material:** ASA for outdoor housings, for UV and heat. PETG is acceptable under full shelter.
    PLA stays indoor-only: it creeps and softens at 55–60 °C.
  - **Conformal coating** on outdoor boards, with keep-outs for the connectors, card fingers and
    sockets, the SHT40, the PIR window, the buttons, the Tag-Connect pads and the test pads.
    Proposed board change B-06: mark the keep-outs on a user layer, so the coating step has a map.
  - **Condensation:** day-night swings condense inside a sealed box. Vent through the membrane rather
    than sealing fully. Give the board a 2–3 mm air gap from the back wall, and keep the low point
    free of copper.
  - **Cold:** the BQ25186 blocks charging below about 0 °C through the pack NTC. Winter solar charge
    is therefore lost on cold days. power_budget.py's winter-reserve case should assume no charging
    below 0 °C. Li-ion discharge is fine to about −20 °C, with reduced capacity.
  - **Heat:** a sheltered box still reaches about 45–50 °C in summer shade. Add the PV charging heat
    in U4 (0.72 W, HD-041) and the cell sits near its 45 °C charge limit. The battery bay goes away
    from U4, and the ASA housing is light-coloured.
  - **PIR outdoors:** sun-warmed surfaces, moving foliage and animals cause false triggers. Use a
    narrow lens and mount it angled down; the firmware should not alarm on a single PIR edge
    (rule-level, not hardware).
- **No door-frame mount:** the coplanar four-slot layout and flat wall mount stay. The 136 × 72 mm
  footprint is fine.
- **Printing:** PLA indoors. ASA (or PETG under full shelter) outdoors. M2.5 heat-set inserts
  assumed; if none are on hand, use printed bosses with self-tapping M2.5 screws.

## 6. Questions for the owner (answered above)

1. Which deployments matter for Rev A: door, room, outdoor/shed? Is outdoor in scope at all?
2. Flat wall-mount nodes, or should some mount on a door frame (narrow, about 20–25 mm wide)? A frame
   mount argues for a different card layout, and the current 136 mm-wide assembly won't fit.
3. Printer and material: what are you printing in (PLA, PETG, ASA), and do you have heat-set inserts?
