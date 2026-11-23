"""Thread timing for TL-SN-002's power profiles on the real OpenThread stack (OTNS simulation).

    ~/.cache/talos-tools/ot-ns/.venv-otns/bin/python tools/otns/thread_latency.py [--runs 5] [--out hardware/results/otns.md]

TL-SN-002 runs OpenThread as an MTD. The design budgets (ADR 0019 amendment 2026-11-22) are:
- deep sleep: event to hub within 2 s. The chip resets, then OT re-attaches from its saved
  settings with a Child Update;
- sleepy child: about 100 ms, attached, polling every 250 ms;
- always on: instant.

This measures what the stack does. Topology: a border router (the hub's RCP) and two routers, plus one
node per profile under test. Measured:
1. **Cold attach:** first power-on to the child role (commissioning-time latency).
2. **Wake from deep sleep:** an OT `reset` (power cycle; settings in flash survive) to the child role,
   then the first echo reply from the border router. This is the deep-sleep profile's event-to-hub
   floor, before the firmware's own boot time (~30 ms) and CoAP exchange.
3. **Uplink round trip** for each profile, from 20 pings to the border router: SED with a 250 ms
   poll (sleepy child), and MED (always on).
4. **Parent loss:** the SED's parent router is removed; time until it is the child of another parent
   (it only notices when its data polls go unanswered).

Results go to hardware/results/otns.md. The script fails if wake-to-reply exceeds the 2 s budget.

Setup (once, by the owner: it installs Homebrew packages and builds OpenThread):
    cd ~/.cache/talos-tools/ot-ns && ./script/install-deps && ./script/install && ./script/install-nodes
PROVISIONAL: run against pyOTNS (ot-ns main, 2026-11) on the development Mac; simulated radio only.
"""

from __future__ import annotations

import argparse
import pathlib
import statistics
import sys
import time as wall

try:
    from otns.cli import OTNS
except ImportError:  # pragma: no cover - only without OTNS installed
    sys.exit("pyOTNS not found: run with ~/.cache/talos-tools/ot-ns/.venv-otns/bin/python (see the docstring)")

ROOT = pathlib.Path(__file__).resolve().parents[2]
BUDGET_DEEP_S = 2.0
STEP_S = 0.05
CHILD = {"child"}


def now(ns: OTNS) -> float:
    t = ns.time
    return t() if callable(t) else t


def pings(ns: OTNS) -> list:
    """Ping results since the last call (a property in some pyOTNS versions, a method in others)."""
    p = ns.pings
    return list(p() if callable(p) else p)


def wait_state(ns: OTNS, node: int, states: set[str], limit_s: float) -> float | None:
    """Advance the simulation until `node` is in one of `states`; seconds taken, or None."""
    t0 = now(ns)
    while now(ns) - t0 < limit_s:
        if ns.get_state(node) in states:
            return now(ns) - t0
        ns.go(STEP_S)
    return None


def first_reply(ns: OTNS, src: int, dst: int, limit_s: float) -> float | None:
    """Ping every 100 ms until one comes back; seconds from the first ping to that reply."""
    t0 = now(ns)
    pings(ns)  # clear old results
    while now(ns) - t0 < limit_s:
        try:
            ns.ping(src, dst, addrtype="mleid", datasize=16, count=1)
        except Exception:  # Busy: the previous echo is still outstanding
            pass
        for _ in range(10):  # wait up to 0.5 s for this echo
            ns.go(0.05)
            if pings(ns):
                return now(ns) - t0
    return None


def one_run(seed: int) -> dict:
    ns = OTNS(otns_args=["-seed", str(seed), "-log", "warn"])
    try:
        ns.speed = 1e6  # as fast as the host allows; timing is simulated, not wall-clock
        br = ns.add("br", 100, 100)
        r1 = ns.add("router", 200, 100)
        r2 = ns.add("router", 150, 190)
        ns.go(30)
        sed = ns.add("sed", 250, 150)
        ns.set_poll_period(sed, 0.25)
        med = ns.add("med", 120, 230)
        out: dict = {"cold_attach_s": wait_state(ns, sed, CHILD, 60.0)}
        wait_state(ns, med, CHILD, 60.0)
        ns.go(5)

        # Uplink round trips per profile.
        for node, key in ((sed, "rtt_sleepy_child_s"), (med, "rtt_always_on_s")):
            pings(ns)
            ns.ping(node, br, addrtype="mleid", datasize=32, count=20, interval=1.0)
            ns.go(25)
            delays = [d / 1000.0 for (_, _, _, d) in pings(ns)]  # pyOTNS reports ms
            out[key] = statistics.median(delays) if delays else None

        # Wake from deep sleep: power cycle with settings kept. The firmware enables IPv6 and
        # Thread at every boot (thread_link.cc), as here; then attach and first reply.
        ns.node_cmd(sed, "reset")
        ns.go(0.03)  # the ESP32-H2's own boot to OpenThread start (~30 ms)
        ns.ifconfig_up(sed)
        ns.thread_start(sed)
        attach = wait_state(ns, sed, CHILD, 30.0)
        out["wake_attach_s"] = attach
        reply = first_reply(ns, sed, br, 10.0) if attach is not None else None
        out["wake_first_reply_s"] = 0.03 + attach + reply if reply is not None else None

        # Parent loss: remove whichever router is the SED's parent.
        if ns.get_state(sed) not in CHILD:
            out["parent_loss_reattach_s"] = None
            return out
        parent_lines = ns.node_cmd(sed, "parent")
        candidates = {n: f"{ns.get_rloc16(n):04x}" for n in (br, r1, r2)}
        victim = next((n for n, rloc in candidates.items() if any(rloc in l.lower() for l in parent_lines)), r1)
        victim_rloc = candidates[victim]
        if victim == br:  # keep the border router (the hub): drop a router it could fall back to instead
            out["parent_loss_reattach_s"] = None
            return out
        ns.delete(victim)
        # Time until the SED is a child of another parent (it only notices the loss when polls fail).
        t0 = now(ns)
        out["parent_loss_reattach_s"] = None
        while now(ns) - t0 < 300.0:
            ns.go(0.25)
            if ns.get_state(sed) in CHILD:
                lines = ns.node_cmd(sed, "parent")
                if not any(victim_rloc in l.lower() for l in lines):
                    out["parent_loss_reattach_s"] = now(ns) - t0
                    break
        return out
    finally:
        ns.close()


def fmt(v) -> str:
    return "-" if v is None else ("inf" if v == float("inf") else f"{v:.3f}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--out", default=str(ROOT / "hardware" / "results" / "otns.md"))
    a = ap.parse_args()
    t0 = wall.time()
    runs = [one_run(seed) for seed in range(1, a.runs + 1)]
    keys = ["cold_attach_s", "wake_attach_s", "wake_first_reply_s", "rtt_sleepy_child_s", "rtt_always_on_s",
            "parent_loss_reattach_s"]
    lines = ["# OTNS Thread timing (generated)", "",
             f"`tools/otns/thread_latency.py`, {a.runs} seeded runs, OpenThread in OTNS (simulated radio). "
             "**Simulation, not a measurement:** real radios, walls and the ESP32-H2's own boot time add to it.",
             "", "| Quantity | " + " | ".join(f"seed {i}" for i in range(1, a.runs + 1)) + " | worst |",
             "|---|" + "---:|" * (a.runs + 1)]
    worst = {}
    for k in keys:
        vals = [r.get(k) for r in runs]
        known = [v for v in vals if v is not None]
        worst[k] = max(known) if known else None
        lines.append(f"| {k} | " + " | ".join(fmt(v) for v in vals) + f" | {fmt(worst[k])} |")
    ok = worst["wake_first_reply_s"] is not None and worst["wake_first_reply_s"] <= BUDGET_DEEP_S
    lines += ["", "- `-` for parent loss: in that run the SED's parent was the border router itself, which the "
              "harness never removes (it stands in for the hub).",
              f"- Deep-sleep budget (wake to first reply) {BUDGET_DEEP_S} s: **{'PASS' if ok else 'FAIL'}**",
              f"- Wall time {wall.time() - t0:.0f} s.", ""]
    pathlib.Path(a.out).write_text("\n".join(lines))
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
