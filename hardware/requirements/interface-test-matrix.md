# Interface test matrix (Rev A bring-up; nothing run yet)

| ID | What | How | Pass | Needs |
|---|---|---|---|---|
| IT-01 | Slot I²C isolation | TL-TEST-001 in all slots; enable one mux channel at a time; read EEPROM 0x50 and expander 0x27 | only the selected slot answers | 4 test cards |
| IT-02 | Wake from deep sleep per slot | test-card expander drives WAKE high; node in deep sleep | wake cause = correct GPIO, < 5 ms | dev kit first (HD-004) |
| IT-03 | Simultaneous wakes | two slots raise WAKE within 1 ms | both causes reported or explicit ambiguity (HW-MF-017) | |
| IT-04 | ALERT aggregation | test card pulls ALERT_N | EXP_INT_N wakes, EXP_B reports slot | |
| IT-05 | CS/RESET defaults at power-up | scope CS_N/RESET_N/SW_EN during boot | CS high, RESET high, SW off until firmware | |
| IT-06 | UART mux | loopback TX→RX on test card per slot | bytes echo only on selected slot | |
| IT-07 | Backfeed into unpowered card | AON_n off, branch disabled, other slots active | < 1 µA into the off slot | µA meter |
| IT-08 | Sleep current, 4 reference cards | battery only, deep sleep | ≤ 40 µA (budget estimate 25 µA) | Joulescope-class meter |
| IT-09 | Source switchover | plug/unplug USB with PV + battery | no reset; ST reflects source | scope |
| IT-10 | Battery-absent USB start | no pack, USB only | boots, Thread joins | |
