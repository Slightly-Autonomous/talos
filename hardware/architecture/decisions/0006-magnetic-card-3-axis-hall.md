# HW-0006: Magnetic contact card uses a 3-axis Hall sensor (TMAG5273)

- Status: accepted (provisional, needs bench verification)
- Date: 2026-11-16
- Supersedes: the DRV5032FB choice for TL-MOD-001 in [HW-0001](0001-hardware-family.md)

## Context

The DRV5032FB in SOT-23 only senses the field component normal to the package top (TI SLVSDC7H). On a
plug-in card the card's angle to the door gap is set by the enclosure, which is not designed yet. If
the card ends up edge-on to the wall, most of the magnet's field runs across the package and the
switch would not trip at a useful gap.

## Decision

TL-MOD-001 uses a TMAG5273A1 (TI SLYS045C) 3-axis linear Hall sensor at the card's far edge:

- I²C 0x35 on the slot branch, clear of the root reservations (0x20, 0x21, 0x6A, 0x70) and the ID
  EEPROM (0x50).
- Wake-up-and-sleep mode at 100 ms. Door closed: only the dominant axis is checked, and the sensor
  interrupts when |B| < 1.5 mT. Door open: all three axes are checked, and it interrupts when any
  |B| > 3 mT. Both thresholds are symmetric (THR_HYST = 1), so magnet polarity and angle don't matter.
- INT is open-drain, active-low and latched until the host addresses the device. An SN74AUP1G04
  inverter with a 100 kΩ pull-up turns it into the slot standard's push-pull, idle-low WAKE
  (HW-0002 is unchanged).
- Firmware: `Tmag5273ContactDriver` (module code `0x0103`, manifest `tl_mod_001_magnetic`) reads the
  field, reports the state with its peak in µT, and re-arms after every event.

## Consequences

- Power: about 2.9 µA (EST) with the door closed versus 0.69 µA. The node deep-sleep model goes from
  24.4 µA to 27.1 µA with all four card types fitted, still under HWR-009 (40 µA).
- After each event the sensor sits in standby (0.45 mA) until the host re-arms it. The driver re-arms
  in the same poll and raises a module fault if it can't.
- The configuration is volatile, so `init()` must run after every power-up of the slot rail.
- One more part per card (the inverter) and roughly USD 1 more BOM.
- Wins: orientation-independent sensing, a field magnitude in every report (groundwork for magnet
  tamper detection), and WAKE now idles low in both door states, which removes the mixed-polarity
  wake requirement from HD-004.
- Open: the thresholds are fixed constants until bench tests with the chosen magnet (HD-020).
  SM351LT (in-plane AMR switch) is the fallback if the measured W&S current is too high.
