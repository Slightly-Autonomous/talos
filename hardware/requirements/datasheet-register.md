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
| TMAG5273 | TI SLYS045C | Rev C (Apr 2026) | Table 4-1 pins (1 SCL, 2 GND, 3 TEST→GND, 4 VCC, 5 INT, 6 SDA); Table 6-2 addresses (A1 0x35); ICC_DCM 1.21 µA @5 s / 160 µA @1 ms (1 ch, 3.3 V); ISTANDBY 0.45 mA; INT latched until addressed (§6.3.3); THR_HYST/MAG_THR_DIR (Table 8-4, 8-6); 820 LSB/mT at ±40 mT; decoupling ≥ 0.01 µF (§7.3) |
| SN74AUP1G04 | TI SCES571K | Rev K (Jun 2014) | Pin table (DBV: 1 NC, 2 A, 3 GND, 4 Y, 5 VCC); ICC 0.5 µA max 25 °C, 0.9 µA max |
| LIS2DW12 | ST DS11811 | Rev 8 | Table 1 pins, Vdd 1.62–3.6 V, SAD 001100x (0x18/0x19), decoupling 100 nF + 10 µF, power-down 50 nA |
| EKMB110111x | Panasonic PaPIRs WL reference spec | Rev 7.1 | Dimensions (bottom view Vdd/OUT/GND), Vdd 2.3–4.0 V, Iw 1.0 µA typ, Iout ≤ 100 µA, warm-up ~25 s |
| PESD5V0L5UV | JLCPCB/LCSC parametric listing C84255 only (Nexperia PDF blocked automated access) | 2026-11-17 | VRWM 5 V, IR 5 nA, Cd 16 pF, VBR 6.4 V, SOT-666. **Pinout not read: VERIFY** |
| MEC8-EM / MEC1-EM | Samtec F-226 Micro Edge Card catalog page | F-226 | Key specs table (pitch, positions, 2.3 A/2.2 A), MEC8-EM ordering (10–70 positions per row, -L plating, 1.60 mm card) |
| MCP2221A | Microchip DS20005565E | Rev E (2021) | Table 1-1 pins (SOIC: 1 VDD, 2 GP0, 3 GP1, 4 RST with internal pull-up, 5 URx, 6 UTx, 7 GP2, 8 GP3, 9 SDA, 10 SCL, 11 VUSB, 12 D-, 13 D+, 14 VSS); Registers 1-12..1-15 factory GP designations; §1.6.2.3 VDD = VUSB = 3.3 V; §1.7.1 SSPND/USBCFG/LED pin behaviour; VUSB bypass 0.22–0.47 µF; VBUS capacitance ≤ 10 µF |
| HSEC8-110-01-S-D-EM2 | Samtec HSEC8-1XX-XX-X-D-EMX recommended PCB / stencil / mating-card layout; HSEC8-1XX-XX-X-D-EMXX product drawing | Rev K (2015); Rev M | -EM2 = 1.57 mm host; -01 = 1.57 mm card. Host: 0.50 × 2.00 mm pads at 0.80 mm, mirrored top/bottom; weld tabs 1.00 × 2.00 mm, "D" = 19.70 mm (-10); 1.549 mm locating slot 3.750 mm outboard of pin 1; 5.00 mm keep-out rework area; "E" min 23.20 mm; 0.152 mm stencil. Card: 0.55 mm fingers, 2.80 mm contact zone ending 1.00 mm from the edge, card "D" = 9.25 mm (-10, no slot), 30° chamfer, 30 µin gold 130–200 Knoop. Body 7.80 mm REF beyond the host edge. **Read from drawing geometry, not printed:** the slot depth (4.375 mm) and the 0.80 mm pad setback |
| AP2112K-3.3, SN74LVC1G125 | not read yet | — | Pin maps taken from KiCad stock symbols (SOT-23-5 VIN/GND/EN/NC/VOUT; DBV OE/A/GND/Y/VCC). **VERIFY against datasheets before ordering TL-DBG-001** |

**Not yet sourced:**
- SHT4x idle current and the 24AA025E standby current. Both use conservative placeholders.
- Detailed TPS2121 application values (PR1/CP2 divider, ILIM, SS).
- The USBLC6 and PESD leakage figures.
- HSEC8 mating-cycle rating at 30 µin gold, and a sample socket to confirm the slot depth and pad setback (HD-002).

The raw PDFs were fetched from vendor sites into a local tool cache. They are not committed, because of copyright.
