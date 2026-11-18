# tools

- `devsim/`: virtual devices for the dev property, acting over the hub's device API (development and browser E2E only).
- `dbg/`: host tool for the TL-DBG-001 one-slot USB debug adapter: configure it, switch card rails, read the card's ID EEPROM, scan I²C, watch WAKE/ALERT_N (HW-0007).

Planned: manifest validator CLI, scenario compiler, manufacturing export, release signer (spec §44).
