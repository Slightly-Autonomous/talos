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
python3 -m talos_dbg id               # module code + revision from the ID EEPROM at 0x50
python3 -m talos_dbg scan             # every I2C address that answers
python3 -m talos_dbg power sw on      # switched rail (refused while AON is off)
python3 -m talos_dbg watch            # WAKE / ALERT_N changes, Ctrl-C to stop
python3 -m talos_dbg power all off
```

To measure a card's current, pull the JP1 (AON) or JP2 (SW) shunt and put a PPK2 in ammeter
mode across the header. The card's UART is a USB serial port from the same MCP2221A (CDC). Open
it with any terminal at the card's baud rate. SPI is not bridged: use the test pads.

## Safety rules the tool enforces

- No card command runs on an unconfigured adapter.
- `configure` needs `--no-card-seated`.
- SW only turns on while AON is on, and AON only turns off after SW.
- I²C commands refuse to run while AON is off. An unpowered card is reported as an error, not
  as an empty slot.
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
- what GP0/GP2 do between power-on and the flash load.
