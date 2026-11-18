# HW-0007: HSEC8-EM2 slot socket, and a one-slot USB debug adapter (TL-DBG-001)

- Status: accepted (provisional, needs bench verification)
- Date: 2026-11-17
- Amends: the socket candidate in [HW-0002](0002-sensor-module-interface.md); closes the socket choice in HD-002

## Context

**Socket.** HW-0002 named Samtec MEC8-110-02-L-D-EM2. The coplanar re-survey
([connector-shortlist.md](../../mechanical/connector-shortlist.md), 2026-11-17) found nothing better
than an edge-mount card-edge socket for 20 side-by-side contacts. Pogo pins need a permanent clamp
force that printed plastic creeps under, and magnetic connectors only make sense for a few contacts.
The weak point was the MEC8-EM's only plating, 10 µin gold, which is thin for an outdoor enclosure that
sees humidity and thermal cycling (fretting). JLCPCB stocks no edge-mount socket in this family.

**Debugging.** Each card should be debuggable alone from a computer before it goes into a Sensor Node,
and then several together. The search for an existing adapter found none for this kind of edge-card
slot. Industrial modular systems solve it with a vendor coupler, so Talos builds its own.

## Decision

1. **Slot socket: Samtec HSEC8-110-01-S-D-EM2.** Same straddle-mount concept, 0.80 mm pitch, 2 × 10,
   with 30 µin gold on the contacts (`-S`). Pinout and card are unchanged. JLCPCB lists it
   (C4845888) with 0 stock, so it is bought from Samtec or a distributor and hand-soldered. That is
   an accepted exception to the JLCPCB-stocked rule, recorded as `not_placed` with HD-002. The
   footprint stays a **placeholder** until Samtec's EM2 drawing is in hand.
2. **TL-DBG-001 one-slot USB debug adapter** (2-layer, 36 × 32 mm). It mirrors one main-board slot:
   - the same socket;
   - the same TPS22917 rail switches;
   - pull-ups on the card's own AON rail;
   - the same PESD5V0L5UV ESD.

   The host reaches the card through an MCP2221A bridge (USB to I²C, UART and 4 GPIO, no firmware):

   | MCP2221A pin | Slot signal |
   |---|---|
   | GP0 | AON enable |
   | GP2 | SW enable |
   | GP1 | WAKE input (GPIO level; interrupt-on-change is available) |
   | GP3 | ALERT_N input |
   | UART | UART_TX / UART_RX |
   | I²C | SDA / SCL |

   Deliberate differences from the main board:
   - **Both rails default off** (1 MΩ pull-downs). The host decides when the card powers up.
   - **JP1 and JP2** put each card rail through a 2.54 mm header. Pull the shunt and insert a current
     meter (e.g. Nordic PPK2 in ammeter mode) to measure the card alone.
   - **Bridge TX goes through a 74LVC1G125 powered from the card rail.** Its Ioff keeps an unpowered
     card unpowered.
   - **Card outputs reach the bridge through 1 kΩ.** This bounds contention while the MCP2221A's GP
     pins still have their factory alternate functions.
   - **SPI is not bridged.** SCK, MOSI, MISO and CS_N go to test pads for a logic analyser.
   - **RESET_N is pulled up only.** Power-cycling AON resets the card.
   - **D1 lights when a card is seated** (the card grounds PRESENT_N).
3. **CI holds the adapter to the slot contract.** `check_kicad.py` checks that its socket has the
   same contact roles as TL-SN-001 J4, contact for contact (`HOSTS`). Like every board, it must also
   pass ERC, netlist round-trip, DRC with schematic parity and the JLCPCB sourcing check.

## Consequences

- The cards stay cheap: gold fingers only, no connector on the card.
- Every Sensor Node and every adapter needs hand-soldered sockets until JLCPCB stocks one, or an
  assembler is given consigned parts.
- **Configure the MCP2221A once, with no card seated.** The host tool writes the GP designations to
  its flash. On a factory-fresh chip both card rails come on:
  - GP0 is LED_URx, which idles high.
  - GP2 is USBCFG, which goes high once USB enumerates (DS20005565E §1.7.1.3–1.7.1.4, Registers
    1-12 and 1-14).

  After configuration the settings load from flash at every power-up. The 1 MΩ pull-downs hold the
  rails off until they do. The host tool ([tools/dbg](../../../tools/dbg/README.md)) owns this step
  (`configure --no-card-seated`) and refuses to run card commands on an unconfigured adapter.
- What the adapter cannot test: SPI cards (none exist yet), the main board's I²C mux and UART mux
  paths, and multi-slot interactions. Those still need TL-SN-001 with TL-TEST-001 cartridges
  ([interface test matrix](../../requirements/interface-test-matrix.md)).

## Needs datasheet or bench verification

- **HSEC8-EM2:** the land pattern and host-board thickness (Samtec drawing), and the mating-cycle
  rating at 30 µin.
- **MCP2221A (datasheet DS20005565E read):**
  - Confirmed: VUSB ties to VDD at 3.3 V (§1.6.2.3), RST has an internal pull-up, and GP1 supports
    interrupt-on-change.
  - Still to check on the bench: what GP0/GP2 do between power-on and the flash load, the URx
    input leakage into an unpowered card, and the host tool's I²C completion and NACK handling
    (written from the datasheet, tested only against a model of the chip).
- **AP2112K-3.3:** headroom from a weak USB port (VBUS droop).
- **PESD5V0L5UV:** the pinout. The PESD3V3L5UV symbol is reused, as on the main board.
