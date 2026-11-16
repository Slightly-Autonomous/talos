# HW-0003: Sensor Node integrated power — TPS2121 → BQ25186 → TPS63900, no 5 V in Rev A

**Status:** Proposed (H2 study). Needs EE review and a bench before layout.

## Decision
- **Input selection:** a **TPS2121** priority power mux, with USB-C on IN1 (priority) and the PV jack on IN2. It blocks reverse current, so no cross-feed between sources and no backfeed into the panel. Inputs run 2.8–22 V and are rated 24 V abs max. The PV path adds a series Schottky (reverse polarity) and an SMAJ12A TVS.
- **Charger:** **BQ25186** linear 1S charger with power path. SYS keeps running from IN with an empty or absent battery. VIN_OVP is 18.5 V, and battery-tracking VINDPM gives crude solar-panel loading control. Charge current is software-limited on PV because a linear charger dissipates (VIN − VBAT) × ICHG.
- **Regulation:** **TPS63900** buck-boost, from SYS_PATH (3.0–4.9 V) to V3V3_CORE, 75 nA Iq. Its output current is >400 mA for VIN ≥ 2.0 V. That covers the H2 radio peak (802.15.4 TX at 18 dBm: 123 mA) plus card bursts.
- **Battery:** a protected 1S Li-ion pack with an NTC on the charger TS pin and a keyed 3-pin connector (BAT+, NTC, GND). The chemistry and cell (for example a protected 18650) stay open as HD-006.
- **No 5 V rail in Rev A.** Contact 5 is reserved. A boost from 1S for radar-class cards is a later revision, gated by admission control.

## Alternatives considered
- **A switching charger (e.g. BQ25620)** is better for PV above about 7 V or above 500 mA. It is rejected for Rev A on sleep Iq, part count and layout risk. Revisit if the PV panel spec grows.
- **Two Schottky diodes for ORing** are simple, but they lose about 0.4 V of USB headroom and cannot prioritise sources. Rejected.
- **TPS2116** (1.6–5.5 V) is rejected because a "6 V" PV panel has an open-circuit voltage above 5.5 V.
- **Regulated-USB solar modules** remain a supported second path ("external panel + USB module"). They need no board change.

## Consequences
The mode matrix and the numbers are in `hardware/power/power-tree.md` and `hardware/results/power-budget.md`. Every value there is an estimate until a Rev A board is measured.
