"""Command line for TL-DBG-001.

    python3 -m talos_dbg list
    python3 -m talos_dbg configure --no-card-seated     # once per new adapter, slot empty
    python3 -m talos_dbg status
    python3 -m talos_dbg power aon on | sw on | sw off | aon off | all off
    python3 -m talos_dbg id                              # read the card's ID EEPROM (AON must be on)
    python3 -m talos_dbg scan                            # list I2C addresses that answer
    python3 -m talos_dbg watch                           # print WAKE / ALERT_N changes until Ctrl-C

Run from tools/dbg (or put it on PYTHONPATH). Needs `pip install hidapi`. With more than one
adapter plugged in, pass --serial (from `list`). Every command takes the adapter's lock.
"""

from __future__ import annotations

import argparse
import contextlib
import sys
import time

from .adapter import AdapterError, DebugAdapter, Manifest, NotConfigured
from .mcp2221 import Mcp2221, Mcp2221Error
from .registry import AdapterBusy, lock


def _open_all(m_vid: int, m_pid: int):
    from .hid_backend import HidDevice, enumerate_bridges

    for info in enumerate_bridges(m_vid, m_pid):
        dev = HidDevice(info.path)
        yield info, dev, Mcp2221(dev)


@contextlib.contextmanager
def _session(serial: str | None):
    manifest = Manifest.load()
    found = []
    for info, dev, bridge in _open_all(0x04D8, 0x00DD):
        s = bridge.read_factory_serial()
        if serial is None or s == serial:
            found.append((s, dev, bridge))
        else:
            dev.close()
    if not found:
        raise SystemExit("no MCP2221A found" + (f" with serial {serial}" if serial else ""))
    if len(found) > 1:
        for _, dev, _ in found:
            dev.close()
        raise SystemExit("more than one MCP2221A: pass --serial (see `list`)")
    s, dev, bridge = found[0]
    try:
        with lock(s):
            yield s, DebugAdapter(bridge, manifest)
    finally:
        dev.close()


def cmd_list(_args) -> int:
    rows = 0
    for info, dev, bridge in _open_all(0x04D8, 0x00DD):
        try:
            s = bridge.read_factory_serial()
            ok = DebugAdapter(bridge).is_configured()
            print(f"{s}  product={bridge.read_product()!r}  {'configured' if ok else 'NOT CONFIGURED'}")
            rows += 1
        finally:
            dev.close()
    if not rows:
        print("no MCP2221A found")
    return 0


def cmd_configure(args) -> int:
    with _session(args.serial) as (s, a):
        for step in a.configure(no_card_seated=args.no_card_seated):
            print(f"{s}: {step}")
    return 0


def cmd_status(args) -> int:
    with _session(args.serial) as (s, a):
        problems = a.config_problems()
        if problems:
            print(f"{s}: NOT CONFIGURED")
            for p in problems:
                print(f"  {p}")
            return 1
        ln = a.lines()
        print(f"{s}: configured  AON={'on' if ln.aon_on else 'off'}  SW={'on' if ln.sw_on else 'off'}  "
              f"WAKE={int(ln.wake)}  ALERT={'asserted' if ln.alert_asserted else 'idle'}")
    return 0


def cmd_power(args) -> int:
    with _session(args.serial) as (s, a):
        if args.rail == "all":
            if args.state != "off":
                raise SystemExit("`power all` only turns everything off")
            a.all_off()
        else:
            a.set_rail(args.rail, args.state == "on")
        ln = a.lines()
        print(f"{s}: AON={'on' if ln.aon_on else 'off'}  SW={'on' if ln.sw_on else 'off'}")
    return 0


def cmd_id(args) -> int:
    with _session(args.serial) as (s, a):
        cid = a.card_id()
        if cid is None:
            print(f"{s}: no ID EEPROM answered at 0x{a.m.id_eeprom:02X} (slot empty, or card not seated)")
            return 1
        print(f"{s}: {cid.describe()}")
        return 0 if cid.manifest_id else 1


def cmd_scan(args) -> int:
    with _session(args.serial) as (s, a):
        found = a.scan()
        print(f"{s}: " + (" ".join(f"0x{x:02X}" for x in found) or "nothing answered"))
    return 0


def cmd_watch(args) -> int:
    with _session(args.serial) as (s, a):
        last = None
        try:
            while True:
                ln = a.lines()
                now = (ln.wake, ln.alert_asserted)
                if now != last:
                    print(f"{time.strftime('%H:%M:%S')} WAKE={int(ln.wake)} "
                          f"ALERT={'asserted' if ln.alert_asserted else 'idle'}", flush=True)
                    last = now
                time.sleep(args.interval_ms / 1000)
        except KeyboardInterrupt:
            return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="talos_dbg", description="TL-DBG-001 debug adapter")
    p.add_argument("--serial", help="MCP2221A factory serial (see `list`)")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list").set_defaults(fn=cmd_list)
    c = sub.add_parser("configure")
    c.add_argument("--no-card-seated", action="store_true",
                   help="confirm the slot is empty (required)")
    c.set_defaults(fn=cmd_configure)
    sub.add_parser("status").set_defaults(fn=cmd_status)
    pw = sub.add_parser("power")
    pw.add_argument("rail", choices=["aon", "sw", "all"])
    pw.add_argument("state", choices=["on", "off"])
    pw.set_defaults(fn=cmd_power)
    sub.add_parser("id").set_defaults(fn=cmd_id)
    sub.add_parser("scan").set_defaults(fn=cmd_scan)
    w = sub.add_parser("watch")
    w.add_argument("--interval-ms", type=int, default=20)
    w.set_defaults(fn=cmd_watch)
    args = p.parse_args(argv)
    try:
        return args.fn(args)
    except (NotConfigured, AdapterError, AdapterBusy, Mcp2221Error) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
