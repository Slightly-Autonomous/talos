"""python3 -m talos_bench --port /dev/cu.usbmodem1101 --expect 0=TL-TEST-001 1=TL-MOD-003

Runs the node bench plan and writes one JSON line per test to --out (default
~/talos-bench/<timestamp>.jsonl, outside the repository), then a summary line.
Exit status is 0 only if nothing failed.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "hil"))

from talos_hil.console import Console, ProcessTransport, SerialTransport, TcpTransport  # noqa: E402

from .runner import CARD_CODES, Bench, plan, summary  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="talos_bench", description=__doc__.split("\n")[0])
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--port", help="the node's USB serial port")
    src.add_argument("--console", help="host:port of a running serial_bridge console")
    src.add_argument("--exec", nargs=argparse.REMAINDER, help="a virtual node: bench_node ...")
    p.add_argument("--expect", nargs="*", default=[], metavar="SLOT=CARD",
                   help=f"cards seated per slot, from {sorted(CARD_CODES)}")
    p.add_argument("--out", help="results file (JSON lines)")
    args = p.parse_args(argv)
    expect = {}
    for item in args.expect:
        slot, _, card = item.partition("=")
        if card not in CARD_CODES or slot not in "0123" or len(slot) != 1:
            p.error(f"bad --expect {item!r}")
        expect[int(slot)] = card
    if args.port:
        transport = SerialTransport(args.port)
    elif args.console:
        host, port = args.console.rsplit(":", 1)
        transport = TcpTransport(host, int(port))
    else:
        transport = ProcessTransport(args.exec)
    out = pathlib.Path(args.out or os.path.expanduser(f"~/talos-bench/{time.strftime('%Y%m%d-%H%M%S')}.jsonl"))
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w") as f:
        def sink(r):
            line = r.to_json()
            f.write(line + "\n")
            mark = {"pass": "PASS", "fail": "FAIL", "blocked": "BLOCKED", "manual": "MANUAL"}[r.verdict]
            print(f"{mark:8} {r.test:24} {r.diagnosis}", flush=True)

        bench = Bench(Console(transport), expect, sink)
        results = bench.run(plan(expect))
        import json
        s = summary(results)
        f.write(json.dumps({"summary": s}) + "\n")
    transport.close()
    print(f"results: {out}")
    return 0 if not s["failed"] else 1


if __name__ == "__main__":
    sys.exit(main())
