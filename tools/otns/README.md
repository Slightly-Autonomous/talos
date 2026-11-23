# OTNS harness (Thread timing)

`thread_latency.py` runs the real OpenThread stack in [OTNS](https://github.com/openthread/ot-ns) to
measure what TL-SN-002's power profiles can achieve:
- cold attach;
- wake from deep sleep to the first reply (the ≤ 2 s budget);
- uplink round trip as a sleepy child (250 ms poll) and always on;
- re-attach after the parent router disappears.

It writes `hardware/results/otns.md` and exits non-zero if the deep-sleep budget fails.

**Status: runs locally** (2026-11-22, results in `hardware/results/otns.md`): deep-sleep wake to
first reply is 0.23–0.33 s in simulation, against the 2 s budget. One-time install, done by the owner;
Homebrew couldn't install `cmake` on this Mac, so a pip-installed one goes first on the PATH:

```bash
/usr/local/bin/python3.12 -m venv ~/.cache/talos-tools/venv-cmake && ~/.cache/talos-tools/venv-cmake/bin/pip install cmake
cd ~/.cache/talos-tools/ot-ns && export PATH="$HOME/.cache/talos-tools/venv-cmake/bin:$PATH" && ./script/install-deps && ./script/install && ./script/install-nodes
```

```bash
cd ~/.cache/talos-tools/ot-ns && PATH="$HOME/go/bin:$PATH" .venv-otns/bin/python ~/Desktop/talos/tools/otns/thread_latency.py --runs 5 --out ~/Desktop/talos/hardware/results/otns.md
```

The repository was cloned to `~/.cache/talos-tools/ot-ns` (ot-ns main, 2026-11). This is a local tool
like the router, not part of CI.

## What it does not cover

- The ESP32-H2's own boot (about 30 ms), the CoAP exchange and the hub's processing. The firmware's
  sim tests cover those.
- Real radio conditions: walls, the housing and metal door frames. Bench: Thread range, plus the
  antenna in the housing.
