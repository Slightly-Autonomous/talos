# Zero-touch assembly plan (proposal, 2026-11-21)

**Owner goal:** boards arrive 100 % assembled. You unbox them, plug them in, and they work. Flashing
firmware over USB is acceptable to begin with, until it is automated. **Nothing here is applied
yet.**

Three research passes fed this plan. Stock figures come from the JLCPCB API on 2026-11-21. Prices
are from public JLCPCB, PCBWay and other vendor pages, and every estimate is marked as one.

## Why today's design isn't zero-touch

| Part | Problem | Today |
|---|---|---|
| Samtec HSEC8-110-01-S-D-EM2 slot socket ×4 (+1 on TL-DBG-001) | 0 in stock at JLCPCB. It straddles the board edge, with pads on **both** sides. No assembler documents reflowing that; it would be hand-soldered. It also clashes with the 5 mm edge rails JLCPCB requires for double-sided builds. | hand-solder (HD-002) |
| ESP32-H2-MINI-1-H4S | JLCPCB stock is 1 | consign or source (HD-023) |
| Panasonic EKMB PIR | 0 across the whole series | open (HD-022) |
| Keystone 5001 loops | thought to be unstocked | hand-fit (HD-042) |

**Key fact:** JLCPCB already does through-hole assembly, at about $3.58 per order plus $0.016 per
joint. A stocked through-hole part therefore still means zero soldering for the owner.

## Proposed changes

| # | Change | JLCPCB part, stock | Effect | Effort |
|---|---|---|---|---|
| Z-1 | **Slot socket:** HSEC8-110-01-**L-RA**-TR, the same Samtec family in right-angle form | C6269126, 95 | Single-side SMD; JLCPCB places it. The card sits parallel to the main board, a few mm above it, still pointing outward. Same 20 contacts, so the slot contract, firmware and cards are unchanged, apart from re-checking the card tongue against Samtec's RA drawing. Costs: 10 µin gold instead of 30 µin, and thin stock (enough for about 19 five-slot builds). | footprint, gen_pcbs, docs |
| Z-1 alt | HSEC8-110-01-S-**DV**-A (vertical, 30 µin) | C4572809, 137 (+93, +20) | The card stands upright, so the housing gets about 30 mm deeper and the PIR/door cards need their sensor turned 90°. | same |
| Z-1 Rev B | M.2 key-E socket, with cards in M.2 form | C5068501 (987) and others, many vendors | Deep, cheap stock, and the keyed notch makes flipped cards impossible. But it's a new card format (0.8 mm, 22 mm wide), every card is redone, and sockets are rated for only about 25–60 insertions. | large |
| Z-2 | **PIR:** NEWOPTO PIR223D-S0 (TO-5, JLCPCB through-hole) | C2887901, 3085 | About 10 µA instead of 1 µA, so sleep current goes from 32.1 to about 40.5 µA, 0.5 µA over HWR-009. **Recover it** by raising R99 (tamper pull-up) from 1 MΩ to 2.2 MΩ: −1.8 µA, and the tamper edge stays well inside 100 ms (2.2 MΩ × 10 nF → about 26 ms to VIH, about 76 ms alarm). Needs a Fresnel lens in the housing, which snaps in. SMD candidate for later: Senba S18-L262B-2 (C2758472, 452), datasheet not yet seen. | card generator, PIR driver timing, power budget |
| Z-3 | **Radio:** keep the ESP32-H2-MINI-1-H4S and have JLCPCB **global-source** it (DigiKey has about 412) | via global sourcing | Zero steps with no redesign. Costs about 4–6 weeks of sourcing lead time, a small price difference, and possible module duty. | none |
| Z-3 alt | ESP32-C6-MINI-1-H4 (PCB antenna, Thread-capable) | C6553337, 1524 | Fully stocked, but it's a real port: different pins, and deep-sleep wake works only on LP GPIO0–7, so the five wake lines move. Rev B fallback if the H2 dries up. | firmware, pins, footprint |
| Z-3 no | ESP32-H2-MINI-1U (U.FL) | C49450282, 560 | Someone has to plug in an antenna cable, so it is not zero-touch. | – |
| Z-4 | **Ground loops:** Keystone 5001 is stocked | C238122, 1067 | JLCPCB through-hole; just fill in the sourcing row. Closes HD-042. | trivial |
| Z-5 | Second sources for thin-stock parts: PESD5V0L5UV → TECH PUBLIC C3021130 (11,630), after a pinout check. MCP2221A QFN C623181 (463). Watch TS3A5017 stock. | | lowers the risk of a stock-out | small |

## Firmware

Every module ships blank, and Espressif only pre-flashes at reel quantities. For now: plug the main
board into USB-C and run one command. The ESP32's built-in USB-Serial/JTAG means no adapter is
needed. Next step: a `talos flash` script that also writes the board identity (`program-id`) and runs
the bench self-test. JLCPCB can flash during assembly for about $16–25 per batch once the image is
stable.

## Rough cost, 5 sets (all estimates; US import duty excluded)

Every board is single-sided SMD plus JLCPCB through-hole. There is no double-sided setup and no
manual straddle soldering.
- **Main boards ×5:** setup, stencil and feeders about $110. Parts about $150, plus sockets about
  $100–160, plus global-sourced modules about $40. PCB about $30, shipping about $35. **About
  $470–530.**
- **Cards ×5 of each of the 4 types, plus TL-TEST-001 and TL-DBG-001:** about $300–600 depending on
  the gold-finger price, which only shows in JLCPCB's quote tool.
- **Total about $0.8k–1.1k plus duty.** US duty on China-built boards is currently 25–50 % of the
  declared value, and the exact rate needs checking at order time.
- **For comparison:** the current design built 100 % by PCBWay or by JLCPCB with hand-soldering
  comes to about $1.6k–3.6k with duty. A US assembler (MacroFab) is about $4k–10k.

## Still needs a JLCPCB quote

These need the owner's account; nothing is ordered.
- Gold-finger price and plated thickness for the cards.
- That the global-sourcing tool finds the H4S module, and at what price.
- Current US duty for this order.
