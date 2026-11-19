# tools/hil: talk to a Talos node from the Mac

The node's USB port (USB-serial-JTAG) carries three kinds of line ([ADR 0016](../../docs/decisions/0016-hardware-bring-up-and-hil.md)):

| Line | Meaning |
|---|---|
| `@T {json}` | diag console replies; each command ends with `{"done":true,"ok":...}` |
| `@F <base64>` | one CoAP datagram to or from the hub (until Thread carries them) |
| anything else | ESP-IDF log lines |

`firmware/sim:bench_node` speaks the same lines on stdin/stdout, so everything here is tested in
CI against the virtual board before it touches hardware.

## Connect a node to a hub

```bash
pip install pyserial
bazel run //server:talos_hub -- --config $PWD/deploy/config/hub.dev.yaml
cd tools/hil
python3 -m talos_hil.serial_bridge --port /dev/cu.usbmodem1101 --hub 127.0.0.1:5683 --console 7770
```

Provision the node once (stored in NVS; the dev keys in `deploy/config/dev-device-keys.yaml` are
public and only for the bench):

```bash
printf 'provision node-12 101112131415161718191a1b1c1d1e1f\n' | nc 127.0.0.1 7770
```

It reboots, builds its CoAP link from the stored identity, and its events reach the hub through
the bridge. `identity` shows the device ID and a key fingerprint, never the key. An unprovisioned
node keeps its events queued.

## Without a board

```bash
bazel build //firmware/sim:bench_node
python3 -m talos_hil.serial_bridge --exec $PWD/../../bazel-bin/firmware/sim/bench_node --slot0=TL-MOD-004 \
    --provision=node-12:101112131415161718191a1b1c1d1e1f
```

## Tests

```bash
bazel build //firmware/sim:bench_node //server:talos_hub
TALOS_BENCH_NODE=$PWD/../../bazel-bin/firmware/sim/bench_node TALOS_HUB=$PWD/../../bazel-bin/server/talos_hub \
    python3 -m unittest discover -s tests -t . -v
```

Without the two variables the end-to-end test is skipped and the bridge unit tests still run.
