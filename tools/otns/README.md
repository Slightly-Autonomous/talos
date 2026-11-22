# OTNS harness (Thread timing)

`thread_latency.py` runs the real OpenThread stack in [OTNS](https://github.com/openthread/ot-ns) to
measure what TL-SN-002's power profiles can achieve:
- cold attach;
- wake from deep sleep to the first reply (the ≤ 2 s budget);
- uplink round trip as a sleepy child (250 ms poll) and always on;
- re-attach after the parent router disappears.

It writes `hardware/results/otns.md` and exits non-zero if the deep-sleep budget fails.

**Status: written, not yet run.** Installing OTNS needs Homebrew packages and builds OpenThread. That
step was refused by this session's safety settings, so the owner runs it once:

```bash
cd ~/.cache/talos-tools/ot-ns && ./script/install-deps && ./script/install && ./script/install-nodes
```

```bash
~/.cache/talos-tools/ot-ns/.venv-otns/bin/python tools/otns/thread_latency.py --runs 5
```

The repository was cloned to `~/.cache/talos-tools/ot-ns` (ot-ns main, 2026-11). This is a local tool
like the router, not part of CI.

## What it does not cover

- The ESP32-H2's own boot (about 30 ms), the CoAP exchange and the hub's processing. The firmware's
  sim tests cover those.
- Real radio conditions: walls, the housing and metal door frames. Bench: Thread range, plus the
  antenna in the housing.
