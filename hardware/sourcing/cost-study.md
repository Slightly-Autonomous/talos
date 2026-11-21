# Single-board Sensor Node: cost-minimisation study (2026-11-21)

**Status:** study for the owner's decision. Nothing is applied: no schematic, manifest, ADR or firmware has
changed, and nothing has been ordered.

**Owner decision this answers (2026-11-21):** the modular 4-slot TL-SN-001 is dropped. Each node becomes one
self-contained board, fully assembled by JLCPCB. Before any redesign, find the **cheapest viable design** that
still meets these:

- Thread/802.15.4 to the existing hub, with ESP-IDF firmware.
- Months to years on battery, with deep sleep in the tens of µA (HWR-009: under 40 µA).
- Indoor nodes and sheltered-outdoor nodes.
- Magnet (door/window), vibration, temperature/humidity and PIR sensing, plus a tamper input.
- Alarm-grade security.

## How to read the numbers

Every figure carries one of these tags:

| Tag | Meaning |
|---|---|
| **[API]** | Read from JLCPCB's public parts API on 2026-11-21 (`selectSmtComponentList`). The stock and price ladder are real but move daily. |
| **[DS]** | Read from the datasheet during this study. Only the ESP32-H2-MINI-1/1U datasheet v1.6 was read (§6 Tables 10, 11, 14 and 16). |
| **[REPO]** | Taken from the repo (`power_budget.py`, datasheet-register.md), with the SRC/EST label it has there. |
| **[LIST]** | Parametric text in the JLCPCB/LCSC listing, such as "1uA" or "10uA". It comes from the vendor, not from a datasheet anyone here has read. Verify it. |
| **[ASSUMED]** | An estimate: cell prices and capacities, PCB and shipping prices, the global-sourcing markup, duty. |

**Fees** (from the brief, JLCPCB price page, Oct 2026):
- **Economic PCBA**, for orders of 5 and 50: setup $8.18, stencil $1.53, plus $3.07 per Extended part type.
- **Standard PCBA**, for orders of 500: setup $25.56, stencil $8.21, plus $1.53 per unique Extended part type.
  The brief's "per unique part" is read here as Extended only, because Basic parts carry no feeder fee. If
  JLCPCB charges it on every unique part, add $1.53 × (number of Basic types) ÷ 500, about $0.05 per node.
- **Joints:** SMT $0.0016 each. Through-hole $0.0164 each, plus $3.58 per order.
- **Module:** the global-sourced module is counted as one Extended type.
- **Preferred Extended parts** (TP4056 C16581, 10 MΩ 0402 C26082): treated as fee-free in Economic PCBA
  [ASSUMED, JLCPCB policy; verify].

**PCB prices** [ASSUMED, from JLCPCB price habits, not quoted]:
- Board: 2-layer, 1.6 mm, lead-free HASL, about 60 × 45 mm.
- Cost: about $4 for 5, $12 for 50 and $65 for 500. A larger 18650 board is about $5, $18 and $95.

**Shipping** [ASSUMED]: about $25, $35 and $90 per order. It is shown separately.

**US import duty:** shown separately at 25 % and 50 % of landed value [ASSUMED rate band; check at order
time].

## 1. Headline: per-node cost

**Definitions:**
- **Ex-works** = parts + amortised PCBA fees and joints + PCB.
- **Landed** = ex-works + shipping.
- **Duty** is listed below the table and is not in either figure.
- The battery, panel and housing are listed after the table, also not included.

| Design | 5 nodes ex-works | 5 nodes landed | 50 ex-works | 50 landed | 500 ex-works | 500 landed |
|---|---:|---:|---:|---:|---:|---:|
| **A. Indoor floor** (LDO + 1 × Li-SOCl₂ AA; DRV5032 / QMA6100P / HDC2010; no PIR) | $13.52 | $18.52 | $6.34 | $7.04 | $4.75 | $4.93 |
| **B. One board for both, recommended** (LDO + 1 × Li-SOCl₂ AA; TMAG5273A2 / LIS2DW12 / SHT40 / PIR223D) | **$16.56** | **$21.56** | **$8.32** | **$9.02** | **$6.40** | **$6.58** |
| B′. Same sensors with TPS63900 buck-boost + 2 × AA (alkaline or lithium) | $21.16 | $26.16 | $10.27 | $10.97 | $7.82 | $8.00 |
| C. Outdoor solar (TPS63900 + 18650 Li-ion + TP4056 + DW01A/FS8205A + PV jack) | $23.32 | $28.32 | $10.27 | $10.97 | $7.59 | $7.77 |

**What makes up each row:**

| Design | Parts | PCBA fees + joints | PCB | Extended types | Joints (SMT / THT) |
|---|---|---|---|---:|---:|
| A | $6.75 / 5.27 / 4.28 | $5.98 / 0.82 / 0.34 | $0.80 / 0.24 / 0.13 | 5 | 136 / 2 |
| **B** | **$9.12 / 7.14 / 5.88** | **$6.64 / 0.94 / 0.40** | $0.80 / 0.24 / 0.13 | 6 | 138 / 5 |
| B′ | $11.26 / 8.83 / 7.27 | $9.10 / 1.18 / 0.41 | $0.80 / 0.26 / 0.14 | 10 | 158 / 3 |
| C | $10.69 / 8.41 / 6.91 | $11.63 / 1.50 / 0.50 | $1.00 / 0.36 / 0.19 | 14 | 185 / 5 |

Values in each cell are at 5 / 50 / 500 nodes.

**Duty** per node on design B: +$5.39 to $10.78 at 5 nodes, +$2.26 to $4.51 at 50, +$1.65 to $3.29 at 500
[ASSUMED rate].

**Not in the table:**

| Item | Cost [ASSUMED] | Note |
|---|---|---|
| ER14505 Li-SOCl₂ cell (A, B) | ~$2–3 | |
| 2 × AA alkaline (B′) | ~$0.6 | |
| 2 × AA lithium (B′) | ~$3.5 | |
| 18650 cell (C) | ~$3–5 | |
| 1–2 W panel and lead (C) | ~$4–8 | |
| Housing | ~$0.6–1.5 of filament per node | Section 7 |

**Whole-order view for 5 nodes of design B:**
- About $83 ex-works, plus about $25 shipping, plus duty.
- Cells add about $12.
- The zero-touch plan priced the modular design at about $0.8k–1.1k plus duty for 5 sets, so this is roughly
  an 8× reduction.

**What dominates at each quantity:**
- **At 5 nodes, fixed fees are about 40 % of the cost.** Each Extended part type adds $3.07 per order, which
  is $0.61 per node. Fewer Extended types is the biggest lever at hobby quantity.
- **At 500 nodes, parts dominate.** The radio module alone is about 45 %.

## 2. Recommendation: design B, "one board for both"

The design in one paragraph:
- One 2-layer board, single-sided, assembled by JLCPCB with Economic PCBA plus through-hole.
- Radio: ESP32-H2-MINI-1-H4S module, global-sourced by JLCPCB.
- Power: a Basic-library 1 µA LDO fed by one 3.6 V lithium-thionyl-chloride AA cell (ER14505 class) in a
  through-hole AA holder.
- No charger, no PV, no USB-C, no buttons.
- Sensors: 3D Hall (TMAG5273A2), LIS2DW12 accelerometer, SHT40, and the PIR223D-S0 fitted on every board but
  powered from a GPIO, so nodes without a lens window keep it off.
- Tamper: two tamper switches in series.
- Flashing: a Tag-Connect/pogo footprint exposing the H2's USB-Serial/JTAG, with a one-off adapter for the
  owner.
- The same board serves indoor and sheltered-outdoor nodes; only the printed housing differs.

### Why this beats the alternatives

1. **No charger and no PV.** With the existing report model, a node runs for years on one primary cell
   (section 4). PV + Li-ion (design C) costs more per node at every quantity.
   - It adds 8 Extended types: $24.56 per order, about $4.9 per node at 5.
   - It adds about $7–13 of cell and panel.
   - Below 0 °C it cannot charge anyway.
   - It brings the HWR-008/010/011 hazards (charger near a primary cell, NTC, thermal).
2. **LDO, not buck-boost.** The ESP32-H2 module must see 3.0–3.6 V, and 3.6 V is also the absolute maximum
   [DS Tables 10–11].
   - A 3.6 V Li-SOCl₂ cell sits just above the regulation point for almost its whole life, so the Basic
     XC6206 LDO (C5446, $0.11, no feeder fee) is enough.
   - The TPS63900 path costs about $0.95 more in parts and 4 more Extended types ($12.28 per Economic order:
     IC, inductor, and the 36.5 k and 16.2 k CFG resistors).
   - That makes B′ $4.60 per node dearer at 5 and $1.42 at 500. What it buys is the freedom to use cheap
     alkaline cells.
3. **Primary cell choice.** ER14505 gives about 2.2 Ah usable at 3.6 V in one AA-size cell [ASSUMED]:
   - very low self-discharge (about 1 %/yr), -55 to +85 °C, and a flat voltage curve;
   - it is the usual alarm-sensor chemistry, so one board covers indoor and the cold shed.
   - 2 × lithium AA would last about as long, but costs a buck-boost because fresh cells reach about 3.6 V,
     which is the absolute maximum.
4. **Same board everywhere.** At 5 nodes, one BOM beats variants: each extra PCBA order costs about $29 in
   fixed fees. At 50 nodes per variant, two BOMs on the **same PCB** save about $0.37 per node:
   - a door variant without the PIR, and a room variant without the TMAG;
   - they save $65 of parts against $28.64 of extra fees.

   **Recommendation:** at 5, build one BOM; at 50 or more of each type, split the BOM, not the PCB.

### 2.1 Candidate BOM, design B

Prices are [API], in USD each, at the price break for 5, 50 and 500 boards. Stock is [API] from 2026-11-21.

| Ref / function | LCSC | Library | Qty | Stock | @5 | @50 | @500 |
|---|---|---|---:|---:|---:|---:|---:|
| U1 ESP32-H2-MINI-1-H4S (PCB antenna, 4 MB, -40 to 105 °C) | C47967030 | Extended (global source) | 1 | **1** at JLC; DigiKey ~412 | ~4.20 | ~3.41 | ~2.86 |
| U2 LDO 3.3 V XC6206P332MR-G, 1 µA Iq [LIST], 200 mA [LIST] | C5446 | **Basic** | 1 | 425,140 | 0.1357 | 0.1092 | 0.0843 |
| C 47 µF 6.3 V X5R 0805 (cell side + rail bulk) | C16780 | Basic | 2 | 1,112,533 | 0.0743 | 0.0591 | 0.0513 |
| C 10 µF 0603 | C19702 | Basic | 2 | 9,904,319 | 0.0167 | 0.0167 | 0.0118 |
| C 100 nF 0402 | C1525 | Basic | 6 | 33,478,190 | 0.0025 | 0.0025 | 0.0015 |
| C 1 µF 0402 (EN RC) | C52923 | Basic | 1 | 9,173,580 | 0.0055 | 0.0055 | 0.0055 |
| C 1 nF 0402 (tamper RC: 10 MΩ × 1 nF, τ = 10 ms) | C1523 | Basic | 1 | 3,924,260 | 0.0017 | 0.0017 | 0.0017 |
| R 10 k 0402 (EN pull-up, TMAG INT pull-up, spare) | C25744 | Basic | 3 | 34,935,987 | 0.0010 | 0.0010 | 0.0008 |
| R 4.7 k 0402 (I²C pull-ups) | C25900 | Basic | 2 | 12,468,105 | 0.0017 | 0.0017 | 0.0013 |
| R 1 M 0402 (VBAT divider; bottom leg to a GPIO, so 0 µA idle) | C26083 | Basic | 2 | 3,462,611 | 0.0010 | 0.0010 | 0.0008 |
| R 10 M 0603 (tamper pull-up: 0.33 µA while the lid is shut) | C7250 | Basic | 1 | 883,286 | 0.0022 | 0.0022 | 0.0022 |
| R 1 k 0402 (LED; PIR supply RC from GPIO) | C11702 | Basic | 2 | 29,481,841 | 0.0011 | 0.0011 | 0.0009 |
| R 100 k 0402 (Q1 gate) | C25741 | Basic | 1 | 12,000,150 | 0.0009 | 0.0009 | 0.0009 |
| D1 red LED 0603 (20 ms status pulses) | C2286 | Basic | 1 | 8,407,836 | 0.0077 | 0.0077 | 0.0077 |
| Q1 AO3401A P-FET (reversed-cell protection) | C15127 | Basic | 1 | 1,203,423 | 0.0820 | 0.0647 | 0.0485 |
| SW1, SW2 tact switches, lid + wall, in series (normally pressed) | C318884 | Basic | 2 | 713,608 | 0.0203 | 0.0203 | 0.0135 |
| U3 TMAG5273**A2**QDBVT 3D Hall (±133/266 mT range) | C3020565 | Extended | 1 | **495** | 0.8459 | 0.7199 | 0.6479 |
| U4 LIS2DW12TR accelerometer | C189624 | Extended | 1 | 1,910 | 0.9032 | 0.6741 | 0.5580 |
| U5 SHT40-AD1B-R2 temperature/RH | C2909890 | Extended | 1 | 12,814 | 1.3089 | 0.9604 | 0.7199 |
| U6 PIR223D-S0 digital PIR, TO-5 through-hole, 10 µA [LIST], 2.7–3.7 V, -20 to 70 °C [LIST] | C2887901 | Extended | 1 | 3,085 | 0.7510 | 0.5383 | 0.4074 |
| BT1 1 × AA holder, through-hole, listed for "AA/14505" | C20606793 | Extended | 1 | **713** | 0.6218 | 0.4320 | 0.3616 |
| J1 programming footprint (TC2030-NL or 6 × 1.27 mm pogo pads: 3V3, GND, EN, GPIO9, USB D−, D+) | – | board feature | 1 | – | 0 | 0 | 0 |
| Fresnel lens (not assembled; snaps into the room-node housing) | C123326 (SB-F-01) | bought loose | 1 | 546 | ~0.09 | ~0.07 | ~0.06 |

**Notes on the BOM:**
- **Totals:** parts about $9.12 / $7.14 / $5.88 per board, of which about $4.92 / $3.73 / $3.02 is not the
  module.
- **Fee count:** 6 Extended types (module, TMAG, LIS2DW12, SHT40, PIR, holder). Everything else is Basic.
- **Module price** [ASSUMED]: DigiKey about $3.82 for 1 and $2.87 at 100, with a 10 % global-sourcing markup.
  The 500 figure is extrapolated. JLCPCB's own listing ladder is $3.73 / $2.88 / $2.41, but it holds only
  1 in stock.
- **Holder stock:** 713 covers 5 and 50. For 500 nodes, use the C5290181 (2,661 stock, same body family; check
  that it suits an ER14505).
- **TMAG5273A2 stock:** 495 covers 5 and 50. For 500, either wait for a restock or fall back to the A1
  (C3716049, 3,365 stock, $2.34 at 500, +$1.70 per node).

### 2.2 Sleep budget, design B

At the 3.3 V rail. With the LDO, battery current ≈ rail current + LDO Iq.

| Load | µA | Basis |
|---|---:|---|
| ESP32-H2 deep sleep (LP timer + LP memory) | 7.0 | [DS] Table 16 |
| TMAG5273 wake-and-sleep, 100 ms, 1 axis | 2.9 | [REPO] EST (A1 figures; A2 assumed the same) |
| LIS2DW12 low-power wake-up | 1.0 | [REPO] EST |
| SHT40 idle | 0.4 | [LIST] "400 nA" (likely a maximum; typical is lower) |
| Tamper loop shut (3.3 V / 10 MΩ) | 0.33 | design |
| LDO Iq | 1.0 | [LIST] |
| VBAT divider (GPIO-switched), leakage, MLCCs | 0.55 | [ASSUMED] |
| **Total, PIR off** | **13.2** | |
| PIR223D-S0 on (room nodes) | +10 | [LIST] |
| **Total, PIR on** | **23.2** | well under the 40 µA of HWR-009 |

## 3. Cost levers, one by one

### 3.1 Radio

Prices are [API] unless marked. The "@5" column uses the 1–9 price break; "@50" the 30–99 break; "@500" the
500 break or the nearest one.

| Option | LCSC | JLC stock | @5 | @50 | @500 | Trade-off |
|---|---|---:|---:|---:|---:|---|
| **ESP32-H2-MINI-1-H4S, global-sourced** | C47967030 | 1 | ~4.20 | ~3.41 | ~2.86 | No port. Adds 9–20 business days of sourcing lead time [ASSUMED price]. **Chosen.** |
| H2-MINI-1-H4S, if LCSC restocks it | C47967030 | 1 | 3.73 | 2.88 | 2.41 | Same part, cheaper; watch the stock. |
| H2-MINI-1-N4 | C18203134 | 0 | 3.61 | 3.01 | 2.83 | Not orderable (older part, -40 to 85 °C). |
| H2-MINI-1-N2 / -H2 / -H2S | C18202993 / C18203071 / C49308772 | 0 | 4.34 / 13.81 / 7.77 | – | – | Not orderable. |
| H2-MINI-1U-H4S (U.FL) | C49450282 | 560 | 3.87 | 2.99 | 2.50 | Stocked, but needs an antenna (~$0.5–1.5 [ASSUMED]) plugged in by hand. That breaks "unbox and plug in", and U.FL is fragile. |
| ESP32-C6-MINI-1-N4 | C5736265 | 6,701 | 3.98 | 3.22 | 2.88 | Stocked and about the same price, so **no saving**. Firmware port: deep-sleep wake only on LP GPIO0–7, plus new BoardSupport pins. Reasonable plan B if H2 supply fails. |
| ESP32-C6-MINI-1-H4 | C6553337 | 1,524 | 4.43 | 3.42 | 3.04 | As above; dearer. |
| **Bare ESP32-H2FH4** + 32 MHz crystal (C9009, $0.07) + matching network + PCB antenna | C22470214 | 1,582 | 2.07 + ~0.15 | 1.64 + ~0.12 | 1.41 + ~0.10 | Saves about $1.4–2 per node in parts, but adds 1–2 Extended types. You lose the module's FCC/IC/CE modular grant: the US §15.23 home-built exemption covers at most 5 units, and certification is roughly $5–15k [ASSUMED]. You also need RF layout and VNA tuning, and 4-layer is advisable. **Not worth it** below thousands of units. |

The H2 is not stocked at JLCPCB. Global sourcing removes the problem, at the cost of lead time. Recheck LCSC
stock on order day, because a restock saves about $0.5 per node.

### 3.2 Power architecture (the biggest lever)

The constraint, read from the datasheet [DS ESP32-H2-MINI-1 v1.6]:
- VDD33 is 3.0–3.6 V recommended (Table 11), and the absolute maximum is **3.6 V** (Table 10).
- The external supply must deliver ≥ 0.35 A (Table 11).
- 802.15.4 TX draws 123 mA at +18 dBm, **54 mA at +7 dBm** and 38 mA at −2 dBm; RX draws 29 mA (Table 14).
- Light sleep is 85 µA, or 25 µA with peripherals powered down. Deep sleep is 7 µA (Table 16).

**Cells** (all capacity and price figures [ASSUMED], typical manufacturer data):

| Cell | Voltage | Usable | Fits 3.0–3.6 V with… | Cell cost | Verdict |
|---|---|---|---|---:|---|
| **1 × ER14505 Li-SOCl₂ (AA size)** | 3.6 V flat; open-circuit ~3.67 V | ~2.2 Ah (derated) | LDO (open-circuit voltage exceeds 3.6 V, so not direct) | ~$2–3 | **Chosen.** Pulse capability is the catch: bobbin cells handle about 100 mA continuous. So cap TX at about +7 dBm (54 mA) and bench-check cold. |
| 2 × AA alkaline | 3.2 → 1.8 V | ~5.1 Wh | Buck-boost only; useless direct (below 3.0 V almost at once) | ~$0.6 | Cheapest cells, but poor below 0 °C and leak-prone over years. B′. |
| 2 × AA lithium FeS₂ | ~3.6 V fresh (open-circuit up to ~3.66 V) | ~8.4 Wh | Buck-boost (fresh pair at or over the absolute maximum) | ~$3.5 | Good outdoors, but needs B′. |
| 3 × AA alkaline | 4.8 → 3.55 V | ~55 % of capacity above the LDO dropout | LDO | ~$1 | Bigger holder, and only half the capacity is usable. No. |
| CR123A (Li-MnO₂) | 3.0 V nominal, sags under TX | ~1.5 Ah | Buck-boost only (below 3.0 V under load) | ~$2 | No. |
| 1 × AA + boost | 1.5 V | ~3.7 Wh | Boost from 0.9 V (Extended IC + inductor) | ~$0.3 | No cheaper than B′, with less energy. |
| 18650 Li-ion + charger + PV | 4.2 → 3.0 V | 8.4 Wh [REPO EST] | Buck-boost + charger + protection | ~$3–5 + panel $4–8 | Design C. No charging below 0 °C; most cost; adds hazards. |
| LiFePO₄ direct | 3.6 V at full charge, 3.2–3.3 V plateau | 18650 ~4.8 Wh | Direct, but the charger's 3.6 V float (±1 %) touches the absolute maximum | cell + CN3058E ($0.19) | No: absolute-maximum margin, and a charger is needed. |

**Regulators** (prices [API]; Iq and dropout [LIST]):

| Regulator | LCSC | Library | @50 | Iq | Verdict |
|---|---|---|---:|---|---|
| **XC6206P332MR-G** | C5446 | **Basic** | 0.109 | 1 µA | **Chosen.** No feeder fee. Dropout at 54–123 mA must be bench-checked against a sagging Li-SOCl₂ cell. |
| HT7333-A | C21583 | Extended | 0.199 | 3.5 µA; 90 mV at 40 mA | Fallback if XC6206 dropout is marginal in the cold. |
| TPS7A0233 DBV / DQN | C5142805 / C2860046 | Extended | 0.604 / 0.417 | 25 nA | Saves about 1 µA (about 5 % of life in deep-sleep mode). Not worth $0.4 plus a fee. |
| TPS63900 + DFE252012F + 36.5 k + 16.2 k | C1518762, C576403, C25887, C27176 | 4 × Extended | 0.746 + 0.121 + ~0 | 75 nA [REPO SRC] | Only for 2 × AA (B′) or Li-ion (C). |
| ME6211C33, RT9013-33, TLV75533, XC6220 | C82942, C47773, C404027, C3040644 | Extended | 0.05–0.16 | 25–60 µA | Rejected: Iq alone breaks HWR-009 in spirit. |
| No regulator | – | – | 0 | 0 | Rejected: no cheap chemistry stays within 3.0–3.6 V for its whole life without risking the absolute maximum. |

### 3.3 Battery life with the existing report model

The model is from `power_budget.py` [REPO EST]:
- 288 check-ins plus 200 events per day, each 40 ms at 30 mA, which averages **6.78 µA** at 3.3 V.
- Self-discharge: Li-SOCl₂ 1 %/yr, alkaline 2.5 %/yr, lithium AA 0.5 %/yr, Li-ion 2 %/month [ASSUMED].
- Buck-boost efficiency 90 % [REPO SRC floor].

Life in years:

| Mode (unverified, HD-004/HD-018) | PIR | B / A: ER14505 + LDO | B′: 2 × AA alkaline | B′: 2 × AA lithium | C: 18650, no sun |
|---|---|---:|---:|---:|---:|
| Deep sleep between reports | off | 11.2 (shelf-limited, ~10) | 6.9 | 12.9 | 3.2 |
| Deep sleep between reports | on | 7.7 | 4.8 | 8.6 | 2.8 |
| Thread SED, light sleep 85 µA | off | **2.5** | 1.6 | 2.7 | 1.6 |
| Thread SED, light sleep 85 µA | on | **2.3** | 1.4 | 2.4 | 1.5 |
| Thread SED, light sleep 25 µA (peripherals off) | off | 6.2 | 3.9 | 6.8 | 2.6 |
| Thread SED, light sleep 25 µA (peripherals off) | on | 5.0 | 3.1 | 5.4 | 2.4 |

**Reading the table:**
- Even in the pessimistic "light-sleep SED" case, one ER14505 lasts about **2.3–2.5 years**. That meets
  "months to years" with no charger.
- PV only wins if the node must run the radio continuously, and no requirement asks for that.
- In design C, self-discharge of the Li-ion pack dominates.

### 3.4 Sensors

Prices [API] at 50 units; currents [LIST] unless marked.

| Function | Option | LCSC | Stock | @50 | Current | Trade-off |
|---|---|---|---:|---:|---|---|
| Magnet | TMAG5273A1 (today) | C3716049 | 3,365 | 2.778 | 2.9 µA W&S [REPO EST] | ±40 mT range, so best resolution. 4× the price of the A2. |
| Magnet | **TMAG5273A2** | C3020565 | 495 | **0.720** | ~2.9 µA (assumed same) | Same die and the existing driver. The ±133/266 mT range is coarser but ample for a 10 × 3 mm N42 magnet at 10–20 mm (verify). Keeps **3D field + magnitude**, so firmware can flag a foreign magnet (magnet-defeat tamper). **Chosen.** |
| Magnet | DRV5032FADBZR, omnipolar switch | C140921 | 59,705 | 0.133 | 1.6 µA, 20 Hz | Cheapest credible option. Any magnet can hold it "closed", so there is no magnet-tamper detection. Saves $0.59. Used in design A. |
| Magnet | DRV5032FB (5 Hz, 0.69 µA) | C2655033 (SOT-23) / C2862715 (TO-92) | 1 / 3,682 | 0.90 / 0.23 | 0.69 µA | The SOT-23 is out of stock; the TO-92 is through-hole. |
| Magnet | KTH1601TL | C962139 | 5,380 | 0.085 | 3.3 µA | Cheaper but higher current, and an unknown brand. |
| Magnet | AH1806 / TLE4913 | C126663 / C126654 | 22k / 3k | 0.22 / 0.36 | 8 / 4 µA | Too much current. |
| Vibration | **LIS2DW12TR** | C189624 | 1,910 | **0.674** | 50 nA power-down; ~1 µA low-power [REPO EST] | Existing driver; the best low-power wake-up. **Chosen.** |
| Vibration | LIS2DW12TR-MS | C50385411 | 11,501 | 0.411 | – | Looks like a second-source copy; register compatibility unknown. |
| Vibration | QMA6100P (QST) | C2887190 | 25,142 | 0.403 | listed "500 nA" | New driver needed; low-power wake-up figures unread. Saves $0.27. Used in design A. |
| Vibration | LIS2DH12TR | C110926 | 9,994 | 0.627 | about 2 µA at 1 Hz, rising with ODR [ASSUMED] | No cheaper than the LIS2DW12, and hungrier. |
| Vibration | LIS3DH / KXTJ3 / MXC4005 / BMA400 / ADXL362 | – | – | 1.39 / 0.51 / 0.38 / 11.8 / 9.4 | – | No gain. SC7A20 is not stocked. |
| T/RH | **SHT40-AD1B-R2** | C2909890 | 12,814 | **0.960** | 0.4 µA idle | Existing driver, DFN, ±0.2 °C. **Chosen.** |
| T/RH | HDC2010YPAR | C477922 | 60,829 | 0.488 | 0.55 µA | Saves $0.47, and has a threshold interrupt. But it is a 0.4 mm-pitch WCSP and needs a new driver. Good at 500 units. Used in design A. |
| T/RH | AHT20 | C2757850 | 26,205 | 0.592 | – | 2.2 V minimum, slow; new driver. |
| T/RH | SHTC3 / SHT30 / HDC1080 / HDC2080 | – | – | 1.69 / 1.19 / 1.53 / 1.31 | – | Dearer. |
| PIR | **PIR223D-S0** (TO-5, through-hole) | C2887901 | 3,085 | **0.538** | 10 µA | Cheapest stocked digital PIR. Powered from a GPIO through 1 k + 10 µF, so nodes without a lens window draw 0. Warm-up after enable, and the -20 °C minimum, need checking. **Chosen.** |
| PIR | Senba S18-L262B-2 (SMD) | C2758472 | 452 | 1.422 | 9.5 µA | Avoids through-hole, but costs $0.88 more. Through-hole is already paid for by the holder. Datasheet not seen. |
| PIR | D203S / AM312 | C78793 / C114881 | 222 / 15 | 0.53 / 0.77 | – | The D203S is analog (needs an amplifier); the AM312 has thin stock. |
| PIR | Panasonic EKMB (1 µA) | – | 0 | – | 1 µA | Not stocked (HD-022). |
| PIR lens | SB-F-01 / SB-F-02 / SY-FL-36BE6 Fresnel | C123326 / C89924 / C22435919 | 546–761 | 0.07–0.09 | – | Not assembled; snaps into the housing. HDPE lenses from other vendors also work. |

**Should every node carry every sensor?**
- Magnet + vibration + T/RH cost about $2.35 per board at 50 (with TMAG A2). A second PCBA order for a
  variant costs about $29 in fixed fees, which is about $5.7 per node across 5 boards.
- So at 5 nodes, put everything on every board, PIR included but GPIO-gated.
- At 50 or more of each type, split the **BOM** (door: no PIR; room: no TMAG), not the PCB (section 2).

### 3.5 Board

| Lever | Choice | Saving / trade-off |
|---|---|---|
| Layers | **2-layer** | The module brings its own RF matching. There is no bare-chip RF and no buck-boost loop. Solid bottom ground plus the antenna keep-out from the module datasheet. 4-layer would add roughly $4 per order at 5 and about $0.35 per node at 500 [ASSUMED]. |
| Size | About 60 × 45 mm | Bounded by the AA holder (~58 × 17 mm), the module and its antenna keep-out, and the PIR. Smaller saves little: PCB is under 3 % of cost at 50. |
| Finish | **Lead-free HASL** | ENIG would flatten the LGA pads under the LIS2DW12 and SHT40 and costs roughly $15–25 per order [ASSUMED]. JLCPCB routinely places 2 × 2 mm LGA on HASL, so ENIG is not needed unless first-article X-ray shows voids. |
| Panelisation | None at 5 or 50; let JLCPCB panelise at 500 | Economic PCBA is single-sided only, so all parts go on top, including the holder (through-hole). |
| Assembly | **Economic** for 5 and 50 (2–50 boards); **Standard** for 500 | Economic fixed cost here is $31.71 per order; Standard is $46.53. Check that Economic accepts through-hole and global-sourced parts; if not, use Standard ($25.56 setup, $1.53 per Extended type). At 5 that adds about $3.3 per node. |
| Extended-part count | 6 types | Each type removed saves $3.07 per order. Candidates: the PIR (two-variant BOM at 50+); swapping the SHT40 for an HDC2010 (still Extended, so no fee saving); swapping the TMAG for a DRV5032 (still Extended, no fee saving). **Every sensor is Extended**: none of these functions exist as Basic parts. |
| Tamper | Two Basic tact switches in series, normally pressed (lid + wall), 10 MΩ / 1 nF | 0.33 µA when shut. τ = 10 ms, inside the 50 ms debounce. The old 1 MΩ loop drew 3.3 µA. A second, separately reported input costs a GPIO and about 2 Basic passives if wanted. |
| LED / buttons | 1 red Basic LED; no RESET or BOOT button | EN, GPIO9 and the USB lines go to the programming pads. No external button means no reset without opening the lid, which trips tamper (H-10). |
| Connectors | **None** | The cell sits in an on-board holder. No battery JST, no PV jack, no USB-C, no tamper JST: the switches are on the board, and the housing has plungers. |
| Reverse cell | AO3401A P-FET, Basic | A single AA can be inserted backwards. |

### 3.6 Firmware flashing

| Option | Per-node cost | One-off cost | Note |
|---|---:|---:|---|
| USB-C 16-pin (TYPE-C-31-M-12 C165948, $0.148) + USBLC6 (C7519, $0.118) + 2 × 5.1 k + VBUS/cell OR-ing | ~$0.30 parts + 2 Extended types ($6.14 per order: +$1.23/node at 5) | 0 | Only worth it for mains-near nodes. The 6-pin USB-C (C456012, $0.038) has **no data lines**, so it is power only. |
| **Pads: TC2030-NL footprint or 6 pogo pads** to the H2 USB-Serial/JTAG (D−/D+), EN, GPIO9, 3V3, GND | **$0** | ~$35–45 Tag-Connect cable, or ~$10 for a DIY pogo clamp + USB cable [ASSUMED] | **Chosen.** Flash with the cell inserted; the adapter carries D+, D− and GND only, so 5 V never reaches the rail. JLCPCB programming in assembly is ~$16–25 per batch later (zero-touch plan). |
| Mains-near nodes | – | ~$5–10 bought "AA battery eliminator" (USB to 3.6 V dummy cell) [ASSUMED] | No board change. |

### 3.7 Housing (note only)

The owner prints in PLA indoors and ASA outdoors. Rough figures [ASSUMED]:
- **Filament:** about 30–50 g per housing at about $20–30 per kg, so **$0.6–1.5 per node**.
- **Hardware:** 4 heat-set inserts (about $0.2), a 10 × 3 mm N42 magnet with its printed carrier (about
  $0.15), and the Fresnel lens for room nodes (about $0.1).
- **Outdoor extras:** an ePTFE vent (about $0.3–1) and a printed TPU gasket.

The earlier deployment notes still apply: wall ≤ 1.5 mm over the Hall sensor, antenna keep-out, and a
separate vent path for the SHT40.

## 4. Requirement trade-offs that design B makes

| Requirement / earlier decision | Effect | Note |
|---|---|---|
| HWR-002/003/004/005/006/013, sensor_edge_v1, cards, ID EEPROM, I²C mux, expanders, slot switches | **Retired** | Fixed sensors on one I²C bus. The firmware discovery/registry becomes a fixed BoardSupport table; ADR 0013 needs an amendment. It is a large simplification and the main saving. |
| HWR-008 (USB-only / battery / PV + battery modes, USB priority) | **Retired** | Battery only. Mains-near nodes use a dummy-cell eliminator. |
| HWR-009 (< 40 µA deep sleep) | Met | Model: 13.2 µA with PIR off, 23.2 µA with PIR on. Restate it at the 3.3 V rail, since the LDO passes battery current through. |
| HWR-010 / HWR-011 (no charger on a primary cell; NTC) | **Moot** | There is no charger at all, which is safer. |
| HWR-012 (flash and test via pads + USB-C) | **Changed** | Pads only, with a one-off adapter. |
| HWR-014, HWR-015 | Unchanged | Pins stay in BoardSupport, provisional. |
| TX power | **Capped at about +7 dBm** (54 mA) instead of +18 dBm | Protects the Li-SOCl₂ cell's pulse rating. About 11 dB less link margin: indoor Thread with mains routers is normally fine, but a distant shed may need a router or a 14500 Li-ion cell in the same holder (it works through the LDO, roughly half its capacity usable). |
| Cell chemistry | One specific primary cell (ER14505 class) | Not stocked in corner shops. A 1.5 V AA simply won't boot, which is safe; a reversed cell is blocked by Q1. Li-SOCl₂ voltage is flat, so low-battery must be judged from **loaded** voltage (firmware change), and warning time is short. Report early. |
| Magnet sensing | TMAG5273**A2** instead of A1 | Coarser resolution. 3D magnet-tamper detection is kept. Design A's DRV5032 would lose it (security-grade argument against design A). |
| PIR | PIR223D-S0 instead of EKMB | 10 µA instead of 1 µA when on. -20 °C minimum [LIST], so a cold outdoor site may need a different part. Needs warm-up after GPIO enable. |
| Tamper | Lid + wall switches in series on one input | Not separately reported unless a second GPIO is used (cheap). |
| Recovery | No buttons | A bricked node needs the lid off and the adapter; OTA rollback (ADR 0013) is the first line. |
| Supply margin | LDO at 3.3 V nominal; module minimum 3.0 V | About 300 mV of headroom for dropout plus cell sag. Bench-check in the cold (section 5). |
| JLCPCB-stocked rule (HWR-018) | The module is global-sourced (stock 1 at JLC) | Needs a risk ID and the owner's OK, as HD-023 already does. |
| Housing | One board, two housings (door/frame vs room wall, with the lens window) | The PIR fitted on door nodes is wasted at 5 units: about $0.6 per node. |

## 5. Unverified: datasheet and bench work

**Datasheets to read before the schematic:**
1. **XC6206P332MR-G** (Torex): Iq, dropout at 30, 54 and 123 mA, stability with 47 µF MLCC, input max 6 V
   (needed for a 14500 Li-ion at 4.2 V).
2. **ER14505 cell** (pick a brand: EVE, Saft LS14500 or Tadiran class): pulse current, voltage under 54 mA at
   -20 °C, passivation/voltage delay, and shipping rules.
3. **PIR223D-S0:** current, output type, warm-up time, supply-noise sensitivity when fed from a GPIO through an
   RC, and lens focal distance.
4. **TMAG5273A2:** sensitivity per range, and W&S current at a 100 ms period (A1 figures were used).
5. **LIS2DW12:** current at the ODR actually needed for vibration (the 1 µA in the repo is EST).
6. **SHT40:** idle current; the 0.4 µA used here comes from the listing.
7. **ESP32-H2 chip datasheet:** the USB-Serial/JTAG pins and the strap behaviour on GPIO9 for the pad
   adapter, plus brown-out threshold settings.
8. **AA holder C20606793:** drawing, and fit and contact pressure with a 14505 cell.

**Bench work:**
- Rail droop at the module during TX at +7 and +18 dBm, from an ER14505 at 25 °C and -20 °C.
- Sleep current of an assembled board (IT-08).
- PIR false triggers when fed from a GPIO.
- Tamper timing.
- The Thread SED duty-cycle cost in light sleep and in deep sleep (HD-004/HD-018). Battery life swings
  2.5 → 11 years on this one assumption.

**Commercial, check at quote time:**
- PCB, ENIG and shipping prices (all [ASSUMED] here).
- Whether Economic PCBA accepts through-hole and global-sourced parts together.
- Whether global-sourced parts incur the feeder fee, and JLCPCB's actual markup on them.
- The Preferred-Extended fee waiver.
- Minimum-purchase and attrition quantities on Extended passives (small).
- The real US duty rate.
- DigiKey's live price and stock for the H4S (412 seen in cached pages).

**Certification:**
- With the Espressif module (which carries its own grant), the node needs only the host-device steps. The
  bare-chip route would need a full radio certification.
- **Security grade:** whether the target standard (for example EN 50131 grade 2 for residential) asks for
  magnet-defeat resistance on door contacts. That is assumed here, and it is why the TMAG A2 is chosen over
  the DRV5032.

## 6. Raw data

API responses from 2026-11-21 were cached locally during the study and are not committed. Rerun
`jlc_sourcing.py refresh` before ordering. Stock and prices move daily: the H2-MINI-1-H4S (1), TMAG5273A2 (495)
and holder C20606793 (713) are the thin lines to watch.
