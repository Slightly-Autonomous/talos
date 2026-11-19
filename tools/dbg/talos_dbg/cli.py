"""Command line for TL-DBG-001.

    python3 -m talos_dbg list
    python3 -m talos_dbg configure --no-card-seated     # once per new adapter, slot empty
    python3 -m talos_dbg status
    python3 -m talos_dbg power aon on | sw on | sw off | aon off | aon cycle | all off
    python3 -m talos_dbg id [--raw]                      # decode the card's ID block (AON must be on)
    python3 -m talos_dbg program-id --card TL-MOD-001    # write a blank card's ID, verify, power-cycle
    python3 -m talos_dbg scan                            # list I2C addresses that answer
    python3 -m talos_dbg reg rd 0x19 0x0F [n] | reg wr 0x19 0x20 0x44 ...
    python3 -m talos_dbg selftest adapter|tl-test-001|tmag|imu|sht
    python3 -m talos_dbg watch                           # WAKE edges (latched) and ALERT changes

Run from tools/dbg (or put it on PYTHONPATH). Needs `pip install hidapi`. With more than one
adapter plugged in, pass --serial (from `list`). Every command takes the adapter's lock.
"""

from __future__ import annotations

import argparse
import contextlib
import sys
import time

import json

from . import id_block, selftest
from .adapter import AdapterError, DebugAdapter, Manifest, NotConfigured
from .mcp2221 import Mcp2221, Mcp2221Error
from .modules import CARDS
from .registry import AdapterBusy, lock

# `id` exit codes, so scripts can tell the cases apart.
ID_OK, ID_EMPTY, ID_BLANK, ID_CORRUPT, ID_UNKNOWN = 0, 1, 3, 4, 5


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
        alert = "unknown (AON off)" if ln.alert_asserted is None else (
            "asserted" if ln.alert_asserted else "idle")
        print(f"{s}: configured  AON={'on' if ln.aon_on else 'off'}  SW={'on' if ln.sw_on else 'off'}  "
              f"WAKE={int(ln.wake)}  ALERT={alert}")
    return 0


def cmd_power(args) -> int:
    with _session(args.serial) as (s, a):
        if args.rail == "all":
            if args.state != "off":
                raise SystemExit("`power all` only turns everything off")
            a.all_off()
        elif args.state == "cycle":
            if args.rail != "aon":
                raise SystemExit("only `power aon cycle` (it takes SW off too)")
            took = a.power_cycle_aon()
            print(f"{s}: AON discharged in {took * 1000:.0f} ms, back on")
        else:
            a.set_rail(args.rail, args.state == "on")
        ln = a.lines()
        print(f"{s}: AON={'on' if ln.aon_on else 'off'}  SW={'on' if ln.sw_on else 'off'}")
    return 0


def cmd_id(args) -> int:
    with _session(args.serial) as (s, a):
        if args.raw:
            low, eui = a.id_raw()
            for off in range(0, len(low), 16):
                print(f"{s}: {off:02X}: {low[off:off + 16].hex(' ')}")
            print(f"{s}: FA: {eui.hex(' ')}  EUI-48 {id_block.eui_text(eui)}")
            return 0
        cid = a.card_id()
        if cid is None:
            print(f"{s}: no ID EEPROM answered at 0x{a.m.id_eeprom:02X} (slot empty, or card not seated)")
            return ID_EMPTY
        print(f"{s}: {cid.describe()}")
        for p in id_block.eui_problems(cid.eui):
            print(f"{s}: warning: {p}")
        if cid.block.status == "blank":
            return ID_BLANK
        if cid.block.status == "corrupt":
            return ID_CORRUPT
        return ID_OK if cid.manifest_id else ID_UNKNOWN


def cmd_program_id(args) -> int:
    with _session(args.serial) as (s, a):
        result = a.program_id(args.card, args.revision, args.expect_current)
        verb = "already programmed" if result.status == "already" else "programmed and verified"
        print(f"{s}: {verb}: {result.card.describe()}")
    return 0


def cmd_scan(args) -> int:
    with _session(args.serial) as (s, a):
        found = a.scan()
        print(f"{s}: " + (" ".join(f"0x{x:02X}" for x in found) or "nothing answered"))
    return 0


def _num(text: str) -> int:
    return int(text, 0)


def cmd_reg(args) -> int:
    with _session(args.serial) as (s, a):
        addr, reg = _num(args.addr), _num(args.reg)
        if args.op == "rd":
            n = _num(args.values[0]) if args.values else 1
            data = a.read(addr, reg, n)
            print(f"{s}: 0x{addr:02X}[0x{reg:02X}] = {data.hex(' ')}")
        else:
            data = bytes(_num(v) for v in args.values)
            if not data:
                raise SystemExit("reg wr needs at least one byte")
            a.write(addr, reg, data)
            print(f"{s}: 0x{addr:02X}[0x{reg:02X}] <- {data.hex(' ')}")
    return 0


def cmd_selftest(args) -> int:
    with _session(args.serial) as (s, a):
        records = selftest.STAGES[args.stage](a)
        for r in records:
            print("@T " + json.dumps(r, separators=(",", ":")))
        failed = [r["t"] for r in records if r["ok"] is False]
        manual = [r["t"] for r in records if r["ok"] is None]
        print("@T " + json.dumps({"cmd": f"selftest {args.stage}", "done": True, "ok": not failed,
                                  "adapter": s, "failed": failed, "manual": manual},
                                 separators=(",", ":")))
        return 0 if not failed else 1


def cmd_watch(args) -> int:
    with _session(args.serial) as (s, a):
        a.enter_wake_latch()  # GP1 edge detector: catches WAKE pulses shorter than a poll
        last_alert = object()
        try:
            while True:
                stamp = time.strftime("%H:%M:%S")
                if a.take_wake_edge():
                    print(f"{stamp} WAKE rose", flush=True)
                alert = a.lines().alert_asserted
                if alert != last_alert:
                    text = "unknown (AON off)" if alert is None else ("asserted" if alert else "idle")
                    print(f"{stamp} ALERT={text}", flush=True)
                    last_alert = alert
                time.sleep(args.interval_ms / 1000)
        except KeyboardInterrupt:
            return 0
        finally:
            a.leave_wake_latch()


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
    pw.add_argument("state", choices=["on", "off", "cycle"])
    pw.set_defaults(fn=cmd_power)
    i = sub.add_parser("id")
    i.add_argument("--raw", action="store_true", help="dump 0x00-0x7F and the EUI-48")
    i.set_defaults(fn=cmd_id)
    pg = sub.add_parser("program-id", help="write a card's ID block (refuses to overwrite)")
    pg.add_argument("--card", required=True, choices=sorted(CARDS))
    pg.add_argument("--revision", type=int, default=1)
    pg.add_argument("--expect-current", help="the card's current 16-byte block in hex, to overwrite it")
    pg.set_defaults(fn=cmd_program_id)
    sub.add_parser("scan").set_defaults(fn=cmd_scan)
    rg = sub.add_parser("reg")
    rg.add_argument("op", choices=["rd", "wr"])
    rg.add_argument("addr")
    rg.add_argument("reg")
    rg.add_argument("values", nargs="*")
    rg.set_defaults(fn=cmd_reg)
    st = sub.add_parser("selftest")
    st.add_argument("stage", choices=sorted(selftest.STAGES))
    st.set_defaults(fn=cmd_selftest)
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
