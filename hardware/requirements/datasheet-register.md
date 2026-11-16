# Datasheet register

These are the sources actually read for H0–H3, with the sections relied on. "Read" means the text was extracted and checked, not a summary. Re-check before layout, because vendors revise datasheets.

| Part | Document | Version read | Sections used |
|---|---|---|---|
| ESP32-H2-MINI-1/1U | Espressif ESP32-H2-MINI-1 & MINI-1U datasheet | v1.6 | §1 features, Table 3 pin definitions, §4 strapping (Tables 4–6), §6 electrical (Tables 10–16), §9 peripheral schematic (EN RC 10 k/1 µF) |
| ESP32-H2 | Espressif ESP32-H2 Series datasheet | v1.3 | §2.2 pin overview (LP pins GPIO8–14 work in any chip mode), power modes, Table "Deep-sleep 7 µA" |
| BQ25186 | TI SLUSF69 | Rev A (Jan 2025) | Table 4-1 pins, §5 electrical (IQ_BAT, VIN_OVP, VINDPM), §6.1 overview (power path, battery-absent), §6.3.16 I²C address 0x6A |
| TPS2121 | TI SLVSEA3F | Rev F (Aug 2020) | §6 pin functions (RUX), §7.1 abs max |
| TPS63900 | TI SLVSET3D | Rev D (Oct 2020) | Features (75 nA), Table 7-2/7-3 R2D settings, §8 typical application (2.2 µH) |
| TPS22917 | TI SLVSDW8B | Rev B (Dec 2021) | Features (IQ 0.5 µA, ISD 10 nA, 1–5.5 V, 2 A), reverse leakage |
| TCA9546A | TI SCPS205B | Rev B | Electrical (standby ICC 1.0 µA typ @3.6 V) |
| TCA9535 | TI SCPS201F | Rev F | Electrical (standby ICC 0.9 µA typ @3.6 V) |
| DRV5032 | TI SLVSDC7H | Rev H (Dec 2024) | Table 4-1 variants, Table 5-1 pins (SOT-23: 1 VCC, 2 OUT, 3 GND), ICC(AVG) FB 0.69 µA @3 V |
| LIS2DW12 | ST DS11811 | Rev 8 | Table 1 pins, Vdd 1.62–3.6 V, SAD 001100x (0x18/0x19), decoupling 100 nF + 10 µF, power-down 50 nA |
| EKMB110111x | Panasonic PaPIRs WL reference spec | Rev 7.1 | Dimensions (bottom view Vdd/OUT/GND), Vdd 2.3–4.0 V, Iw 1.0 µA typ, Iout ≤ 100 µA, warm-up ~25 s |
| MEC8-EM / MEC1-EM | Samtec F-226 Micro Edge Card catalog page | F-226 | Key specs table (pitch, positions, 2.3 A/2.2 A), MEC8-EM ordering (10–70 positions per row, -L plating, 1.60 mm card) |

**Not yet sourced:**
- SHT4x idle current and the 24AA025E standby current. Both use conservative placeholders.
- Detailed TPS2121 application values (PR1/CP2 divider, ILIM, SS).
- The USBLC6 and PESD leakage figures.
- The MEC8-EM2 host-board thickness drawing.

The raw PDFs were fetched from vendor sites into a local tool cache. They are not committed, because of copyright.
