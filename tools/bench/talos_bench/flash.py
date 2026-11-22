"""One command to bring up a TL-SN-002 over its USB-C port (ADR 0019):

    python3 -m talos_bench.flash --port /dev/cu.usbmodem1101 --unit-serial SN2-A-0007

1. flash   the ESP-IDF build (firmware/esp_idf/build, TL-SN-002 is the default Kconfig target)
           through the H2's USB Serial/JTAG: `esptool --chip esp32h2 ... write_flash @flash_args`
           if esptool is installed, else `idf.py -p PORT flash`. Neither found: the step is
           "skipped" (said so, never faked), and the rest still runs on whatever is flashed.
2. lid     OPEN THE ENCLOSURE LID. TL-SN-002's USB console is read-only while the lid is closed
           (ADR 0019 amendment 2026-11-22: the USB-C port is reachable from outside), so the identity
           write needs it open. Opening it raises the tamper alarm: put the node in maintenance on
           the hub first. On a real board the tool asks and waits; on the virtual board it opens it.
3. identity  board name, revision and unit serial into talos_id, write-once and read back
           (talos_dbg program-id --node: the same code, tools/dbg/talos_dbg/node_id.py).
4. self-test the TL-SN-002 bench plan (talos_bench.sn002): sensors, then CLOSE the lid for the
           tamper and console-lock checks (S-M17, S-M20), then open it again (S-M21) for the
           self-tests, lamps, buttons, source, PIR rail and battery. Manual steps stay "manual".

One JSON line per step on stdout, then a summary line; exit 0 only if nothing failed.
--no-flash skips step 1 (a board flashed by JLCPCB at assembly). --dry-run prints the flash
command without running it. --exec runs against firmware/sim:bench_node instead of a port.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import time
from typing import Callable

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "hil"))
sys.path.insert(0, str(ROOT / "tools" / "dbg"))

from talos_dbg import node_id  # noqa: E402
from talos_hil.console import Console, ProcessTransport, SerialTransport, TcpTransport  # noqa: E402

from . import sn002  # noqa: E402
from .runner import Bench, summary  # noqa: E402

DEFAULT_BUILD = ROOT / "firmware" / "esp_idf" / "build"
FLASH_BAUD = 460800


def flash_command(build_dir: pathlib.Path, port: str,
                  which: Callable[[str], str | None] = shutil.which) -> list[str] | None:
    """The command that flashes `build_dir` to `port`, or None if no flasher is installed."""
    args_file = build_dir / "flash_args"  # written by every ESP-IDF build
    for tool in ("esptool.py", "esptool"):
        exe = which(tool)
        if exe and args_file.exists():
            return [exe, "--chip", "esp32h2", "-p", port, "-b", str(FLASH_BAUD),
                    "--before", "default_reset", "--after", "hard_reset", "write_flash", f"@{args_file}"]
    idf = which("idf.py")
    if idf:
        return [idf, "-C", str(build_dir.parent), "-B", str(build_dir), "-p", port, "flash"]
    return None


def step(name: str, verdict: str, detail: dict | None = None, diagnosis: str = "") -> dict:
    line = {"step": name, "verdict": verdict, "detail": detail or {}, "diagnosis": diagnosis}
    print(json.dumps(line, sort_keys=True), flush=True)
    return line


def do_flash(args) -> dict:
    if args.no_flash:
        return step("flash", "skipped", {}, "--no-flash: keeping the image already on the board")
    if not args.port:
        return step("flash", "skipped", {}, "no serial port (virtual or bridged node): nothing to flash")
    cmd = flash_command(pathlib.Path(args.build_dir), args.port)
    if cmd is None:
        return step("flash", "skipped", {"build_dir": args.build_dir},
                    "neither esptool nor idf.py found (or no build/flash_args): install ESP-IDF, "
                    "`cd firmware/esp_idf && idf.py set-target esp32h2 build`, rerun")
    if args.dry_run:
        return step("flash", "skipped", {"command": cmd}, "--dry-run")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return step("flash", "fail", {"command": cmd, "stderr": r.stderr[-800:]},
                    "flashing failed: check the USB-C cable carries data, hold BOOT while pressing RST "
                    "to force download mode, or recover over the Tag-Connect UART0 pads")
    time.sleep(2.0)  # hard reset: the USB Serial/JTAG port re-enumerates
    return step("flash", "pass", {"command": cmd})


def open_transport(args):
    if args.port:
        return SerialTransport(args.port)
    if args.console:
        host, port = args.console.rsplit(":", 1)
        return TcpTransport(host, int(port))
    return ProcessTransport(args.exec)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="talos_bench.flash", description=__doc__.split("\n")[0])
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--port", help="the node's USB-C serial port")
    src.add_argument("--console", help="host:port of a running serial_bridge console")
    src.add_argument("--exec", nargs=argparse.REMAINDER, help="a virtual node: bench_node --board=TL-SN-002 ...")
    p.add_argument("--unit-serial", required=True, help="serial printed on the board's label")
    p.add_argument("--board", default="TL-SN-002", choices=sorted(node_id.BOARDS))
    p.add_argument("--board-revision", default="A")
    p.add_argument("--build-dir", default=str(DEFAULT_BUILD))
    p.add_argument("--no-flash", action="store_true")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args(argv)
    try:
        node_id.validate(args.board, args.board_revision, args.unit_serial)
    except node_id.NodeIdError as e:
        p.error(str(e))

    steps = [do_flash(args)]
    if steps[0]["verdict"] == "fail":
        print(json.dumps({"summary": {"failed": ["flash"]}}), flush=True)
        return 1
    transport = open_transport(args)
    try:
        console = Console(transport)
        bench = Bench(console, {})
        locked = (console.command("id").record("id") or {}).get("console_locked")
        if locked:
            # TL-SN-002 (or any board table with console_lock): the identity write needs the lid open.
            tamper = sn002.lid(bench, True)
            steps.append(step("lid.open", "pass" if tamper == 1 else "fail", {"tamper": tamper},
                              "" if tamper == 1 else
                              "the console is read-only while the enclosure is closed: open the lid "
                              "(start maintenance on the hub first), then rerun"))
        try:
            rec = node_id.program(console, args.board, args.board_revision, args.unit_serial)
            steps.append(step("identity", "pass", {k: rec.get(k) for k in ("board", "rev", "serial")}))
        except Exception as e:  # noqa: BLE001 - every failure is a reported step
            steps.append(step("identity", "fail", {}, str(e)))
        results = []
        if args.board == "TL-SN-002":
            if locked and bench.simulated():
                sn002.lid(bench, False)  # the operator closes it for S-M17/S-M20 (a real board: S-M17 asks)
            results = bench.run(sn002.plan())
            for r in results:
                step(f"selftest.{r.test}", r.verdict, r.detail, r.diagnosis)
    finally:
        transport.close()
    failed = [s["step"] for s in steps if s["verdict"] == "fail"]
    failed += [f["test"] for f in summary(results)["failed"]] if results else []
    print(json.dumps({"summary": {"failed": failed, "flash": steps[0]["verdict"]}}), flush=True)
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
