# Hardware decision and risk register

Status at H3 (2026-11-16). Owner for every row is "EE reviewer" unless it says "bench" (needs hardware) or "owner" (needs a business decision). **Blocking** means it blocks the next board revision (Rev A layout, H4).

## Open decisions (handoff §42 plus new items)

| ID | Decision | State / evidence | Next action | Blocking H4? |
|---|---|---|---|---|
| HD-001 | MCU variant | **Proposed:** ESP32-H2-MINI-1-H4S (4 MB). Pad ledger complete from datasheet v1.6 | EE review; confirm 4 MB fits A/B OTA (ADR 0013) | yes |
| HD-002 | Coplanar connector | **Proposed:** Samtec MEC8-110-02-L-D-EM2; alternates in mechanical/connector-shortlist.md | Owner approval to request samples and a quote; check the EM2 drawing for host-board thickness | yes |
| HD-003 | ADC vs smart analog | **Decided for Rev A:** no slot ADC; contact 19 reserved | Revisit with the leak/piezo card | no |
| HD-004 | Four wake sources | **Proposed:** GPIO10–13 direct, GPIO14 = expander INT. All Rev A cards now wake active-high (HW-0006 removed the contact card's level-following WAKE), so mixed per-pin levels are no longer needed; four-pin ext1 ANY_HIGH on H2 is still unverified | Dev-kit test: four wake pins, active-high, from deep sleep | yes |
| HD-005 | Slot current budgets | AON 20 mA/slot, SW 150 mA/slot *(est)*; TPS22917 2 A max *(src)* | Measure card inrush; set CT capacitors | no |
| HD-006 | Battery pack/chemistry | Protected 1S Li-ion + 10 k NTC assumed | Pick pack; check NTC curve against BQ25186 TS thresholds | yes |
| HD-007 | Charger and source priority | **Proposed:** TPS2121 + BQ25186 + TPS63900 (HW-0003) | EE review of PR1/CP2 divider and ILIM/SS values (marked TBD in schematic) | yes |
| HD-008 | Low-sun reserve | Calculated in results/power-budget.md | Weather traces in the twin; field measurement | no |
| HD-009 | Layer count/side | **First pass:** SN 4-layer (GND/3V3 planes), cards 2-layer, single-sided assembly | Fab DFM review and quote (owner approval) | yes |
| HD-010..014 | Vision Node | Desk study done ([vision-compute-study.md](../architecture/vision-compute-study.md)): CM4 is the low-risk default; RV1106 class is the power option; CM5 has no hardware H.264 encode | Owner approval to buy dev kits; run VB-01..06 | – |
| HD-015 | Manufacturer | Not started | H4 quotes (owner approval) | – |
| HD-016 | Secure provisioning | eFuses untouched (HW-0005) | Owner signing/recovery design | no |
| HD-017 | Certification | Not started | Before sale | no |
| HD-018 | No 32 kHz crystal | GPIO13/14 used for wake, so no XTAL_32K | Measure Thread SED timing drift on the RC slow clock; if bad, move the slot-3 wake to the expander | yes |
| HD-019 | PV panel spec | Assumed "6 V nominal", Voc ≤ 9 V, ≤ 2 W | Owner product decision; it drives the TVS and charger-current policy | no |
| HD-020 | Magnetic card: 3-axis Hall behaviour | TMAG5273 (HW-0006) removes the orientation dependence. New costs: ~2.9 µA W&S at 100 ms (EST, vs 0.69 µA), 100 ms detection latency, 0.45 mA standby after every event until the host re-arms, volatile config after every power-up, fixed 1.5/3.0 mT open/close thresholds | Bench: measure W&S current at 100 ms; confirm thresholds with the chosen magnet and gap at all angles; confirm firmware re-arms within ms of each event and after rail cycles | yes |
| HD-021 | BQ25186 land pattern modified | Pads at ±0.825 mm and EP 0.75 × 1.5 mm (TI example: ±0.8, 0.9 × 1.5) to get 0.2 mm pad-to-EP copper | EE review vs TI SLUSF69 §12; confirm thermal pad solder coverage | yes |

## Risks found while drawing the schematic (need a reviewer)

1. **Ext1 wake semantics on the H2.** If ESP-IDF supports only "any high" or "any low" across the mask, the contact card (normally low, high on open) and future active-low cards need firmware to re-arm the level each time, or need light sleep.
2. **GPIO8 status LED:** the LED plus a 10 k pull-up on a strap pin must still read high at reset. Check t_H = 3 ms hold *(src)*.
3. **TPS2121 PR1/CP2 configuration** was taken from pin descriptions only. The divider values in the schematic (100 k / 33 k) are placeholders, and the ILIM and SS component values are TBD.
4. **SMAJ12A polarity:** the library symbol is two-anode (A1/A2), so confirm the unidirectional orientation at review.
5. **The linear charger dissipates heat on PV:** (VIN − VBAT) × ICHG. Keep ICHG ≤ 300 mA on PV (power study §3).
6. **VBAT pin 15 of the MINI-1** is tied to V3V3_CORE. The module default says it is internally tied to 3V3, so confirm against the module schematic (datasheet §8).
7. **TVS arrays on slot lines:** check PESD3V3L5UV leakage against the sleep budget.
8. **The SPI/UART lines are bused to unpowered cards.** Firmware must hold SCK/MOSI low and the UART mux disabled whenever a slot is unpowered (interface rule S-3).
9. **Footprints:** the MINI-1, DLH0010A, EKMB and card-finger footprints are now drawn from their drawings. The **MEC8-EM2 socket footprint is still a placeholder** (no Samtec drawing obtained), and the EKMB pin mapping needs a physical check.
13. **The H4 layout is autorouted and unreviewed.** It has no impedance control, and nobody has reviewed the RF or power-loop placement. The full list is in `layout/README.md`.
10. **SHT4x idle and 24AA025E standby currents are not yet sourced.** The budget uses conservative 1 µA placeholders.
11. **Floating expander inputs.** `PRESENT_PROBE` floats until firmware drives it, so EXP_B P13 must default to an output driven low. SLOTn_ALERT_N floats while that slot's AON switch is off, because AON has no QOD. Floating CMOS inputs raise TCA9535 current. Options: enable QOD on the AON switches too, or have firmware mask the slot. Decide at review.
12. **C6 and C8 (10 µF/25 V and 4.7 µF/25 V in 0805)** lose most of their capacitance under DC bias at PV voltages. Size them from the vendor's DC-bias curves.
