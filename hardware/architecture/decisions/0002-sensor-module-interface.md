# HW-0002: Sensor module interface `sensor_edge_v1` (20 contacts, coplanar edge socket)

**Status:** Proposed. Needs connector samples and EE review before H4.

## Decision
- **20 contacts at 0.80 mm pitch, 2 rows.** The card is a 1.60 mm PCB with gold fingers that slides into an **edge-mount** socket straddling the main-board edge, so card and main board are coplanar. Socket: Samtec HSEC8-110-01-S-D-EM2 (30 µin gold; chosen over the MEC8-EM in [HW-0007](0007-slot-socket-and-debug-adapter.md)). Alternates are in [`../../mechanical/connector-shortlist.md`](../../mechanical/connector-shortlist.md).
- **Pinout:** see [`../../interfaces/sensor_edge_v1.md`](../../interfaces/sensor_edge_v1.md). It has:
  - 5 grounds
  - always-on and switched 3.3 V per slot
  - reserved 5 V (contact 5, not connected on Rev A)
  - a per-slot isolated I²C branch
  - a dedicated push-pull WAKE line
  - an open-drain ALERT_N
  - shared SPI with per-slot CS_N
  - per-slot muxed UART
  - RESET_N and PRESENT_N
  - a reserved analog contact (19, not connected)
- **Analog (HD-003):** Rev A routes no ADC to the slots. Analog cards (piezo, leak) must digitize or compare on-card and present WAKE/ALERT or I²C. Contact 19 stays reserved so a later revision can add a protected ADC path without changing the connector.
- **Every card carries an ID EEPROM at 0x50 powered from AON.** Discovery reads it through the slot's mux branch. Any card device that touches the branch I²C must run from AON (or sit behind an isolating buffer), because the branch pull-ups go to AON. This rule exists to prevent backfeed into an unpowered chip.
- **No hot-swap in Rev A.** Cards are changed with the node off (handoff §7).

## Consequences
- The validator checks the connector standard (`kConnectorMismatch`), reserved root I²C addresses (`kI2cRootConflict`) and UART concurrency (`kUartOversubscribed`).
- Socket and finger finish are settled by [HW-0007](0007-slot-socket-and-debug-adapter.md): HSEC8 with 30 µin gold, and card fingers in 30 µin hard gold with a 30° chamfer. Only a 9.25 mm tongue of each card enters the socket.
