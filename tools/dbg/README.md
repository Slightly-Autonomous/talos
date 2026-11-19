# tools/dbg: TL-DBG-001 host tool

Drives the [TL-DBG-001](../../hardware/kicad/TL-DBG-001) one-slot USB debug adapter
([HW-0007](../../hardware/architecture/decisions/0007-slot-socket-and-debug-adapter.md)) from a
computer. Pure Python 3.9+, talking to the adapter's MCP2221A over USB HID.

```bash
pip install hidapi
cd tools/dbg
python3 -m talos_dbg list
```

## First use of a new adapter (once)

A factory-fresh MCP2221A turns **both card rails on** (GP0 is LED_URx and idles high; GP2 is
USBCFG and goes high once USB enumerates). So with the **slot empty**:

```bash
python3 -m talos_dbg configure --no-card-seated
```

This loads the run-time GP settings first, so the rails turn off at once. Then it writes the
power-up settings to flash and reads them back: GP0/GP2 become GPIO outputs, low; GP1/GP3 become
GPIO inputs. It also sets the USB product string to `TL-DBG-001`. Every other command refuses
to run until the flash holds these settings.

## Bringing up a card

```bash
python3 -m talos_dbg power aon on     # card's always-on rail (ID EEPROM, I2C pull-ups)
python3 -m talos_dbg id               # decode the ID block v1 (exit 0 ok, 1 empty, 3 blank, 4 corrupt, 5 unknown)
python3 -m talos_dbg id --raw         # dump 0x00-0x7F and the EUI-48
python3 -m talos_dbg scan             # every I2C address that answers
python3 -m talos_dbg reg rd 0x19 0x0F # one register (or `reg wr <addr> <reg> <bytes...>`)
python3 -m talos_dbg power sw on      # switched rail (refused while AON is off)
python3 -m talos_dbg power aon cycle  # really reset the card: waits for AON to discharge
python3 -m talos_dbg watch            # WAKE edges (latched, catches short pulses) and ALERT_N
python3 -m talos_dbg power all off
```

### Programming a card's ID (once per card)

```bash
python3 -m talos_dbg program-id --card TL-MOD-001
```

It refuses unless the card answers on exactly the addresses its manifest lists (so a card is
never named wrongly), the EUI-48 looks factory-programmed, and the ID is blank. Rewriting an ID
needs `--expect-current <the 16 bytes in hex>` from `id --raw`. It writes one 16-byte page,
waits out the write cycle, reads it back, power-cycles AON and reads it again, then appends the
card, EUI and block to `~/.local/share/talos-dbg/card-ids.jsonl` (outside the repo).

### Self-tests (bench-test-plan.md stages 1-3)

```bash
python3 -m talos_dbg selftest adapter       # slot empty: config, line levels, empty bus, AON discharge
python3 -m talos_dbg selftest tl-test-001   # proves the test card before it is trusted as a fixture
python3 -m talos_dbg selftest tmag          # TL-MOD-001: identity and the WAKE path, no magnet
python3 -m talos_dbg selftest imu           # TL-MOD-003: WHO_AM_I, gravity, electrostatic self-test
python3 -m talos_dbg selftest sht           # TL-MOD-004: serial with CRC, heater rise
```

Output is one `@T {json}` line per check, the same shape as the node's diag console, ending
with a done record that lists failed and manual checks. Exit status is 0 only when nothing
failed. `"ok": null` marks a step for a person (meter or PPK2), with the instruction in `manual`.

To measure a card's current, pull the JP1 (AON) or JP2 (SW) shunt and put a PPK2 in ammeter
mode across the header. The card's UART is a USB serial port from the same MCP2221A (CDC). Open
it with any terminal at the card's baud rate. SPI is not bridged: use the test pads.

## Safety rules the tool enforces

- No card command runs on an unconfigured adapter.
- `configure` needs `--no-card-seated`.
- SW only turns on while AON is on, and AON only turns off after SW.
- I²C commands refuse to run while AON is off. An unpowered card is reported as an error, not
  as an empty slot. SDA or SCL held low with AON on is reported as a bus short, not a NACK.
- ALERT_N is reported as unknown while AON is off (its pull-up is on AON).
- `watch` routes GP1 to the edge detector in SRAM only; flash is never touched.
- One process per adapter: an exclusive lock keyed by the MCP2221A factory serial
  (`~/.cache/talos-dbg/locks`). With several adapters plugged in, pick one with `--serial`.

## Tests

```bash
cd tools/dbg && python3 -m unittest discover -s tests -t . -v
```

The tests run the real driver and CLI against `tests/fake_mcp2221.py`, a model of the chip and
slot built from datasheet DS20005565E. They also hold `tl-dbg-001.json` to the adapter
schematic's netlist, and `talos_dbg/modules.py` to the firmware module registry.

## Not verified on hardware

The driver was written from the datasheet and tested only against the model, which uses the same
reading of the datasheet. Check these on the first adapter:

- the I²C completion and NACK detection: status byte 8 and the byte 20 ACK bit, Get I2C Data
  returning 127 on error;
- the factory GP designation codes (the datasheet's read and write tables disagree for GP0; the
  tool only ever writes GPIO, code 000);
- what GP0/GP2 do between power-on and the flash load;
- the SCL/SDA line-level bytes (22/23) and the GP1 edge detector (byte 24, SRAM byte 6);
- whether an idle SHT40 ACKs the one-byte read `scan` uses as a probe;
- the TMAG5273 forced-threshold recipe and the LIS2DW12 self-test step sizes on real parts.
