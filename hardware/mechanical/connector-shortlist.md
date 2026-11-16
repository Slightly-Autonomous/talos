# Coplanar slot connector shortlist (H1, HD-002)

"Coplanar" means the card's plane is the main board's plane: the card slides in sideways. Vertical edge sockets and mezzanine stackers are excluded.

| Rank | Family / example MPN | Type | Pitch / contacts | Card / host | Rating *(src: Samtec F-226)* | Pros | Cons / to verify |
|---|---|---|---|---|---|---|---|
| 1 | **Samtec MEC8-EM** (MEC8-110-02-L-D-EM2) | Edge-mount edge-card socket, straddles host edge | 0.80 mm, 10–70 per row (2 × 10 = 20) | 1.60 mm card | 2.3 A (2 pins), 180 VAC, −55 to +125 °C | Card fingers cost nothing (only plating); thin; true coplanar | Host-board thickness for EM2 tails unverified; "non-standard, non-returnable" options; 0.25 µm gold vs card hard-gold; insertion cycles not on catalog page |
| 2 | **Samtec MEC1-EM** (MEC1-110-02-L-D-EM2) | Same, 1.00 mm | 1.00 mm, 2 × 10 = 20 | 1.60 mm card | 2.2 A (2 pins), 250 VAC | Wider pitch, easier finger plating and alignment | ~25 % longer; same open questions |
| 3 | **Right-angle terminal + right-angle socket pair** (Samtec coplanar right-angle family; Adam Tech ESMC 1.27 mm as second source) | Mated connector pair, both right-angle, boards edge to edge | 1.27 mm typ | any thickness | per series | Retention and cycles are standard header specs; no finger plating | Two connectors per slot (cost); exact coplanar MPN pair **not yet identified**, must be chosen from the vendor's coplanar guide |

**Recommendation:** MEC8-EM, with MEC1-EM as the drop-in alternate if finger alignment or plating tolerance is a problem. Both use the same 20-contact logical pinout, so the schematics don't change.

**Before H4 (owner approval needed for samples and quotes):**
1. Get the EM2 drawing and confirm host-board thickness and keepout.
2. Get samples and test 1.60 mm ±10 % cards, insertion force and retention in the printed guide.
3. Get a quote for card hard-gold vs ENIG fingers.
4. Make a 3D fit with the enclosure, keeping the antenna keepout.
