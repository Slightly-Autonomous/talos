# TL-VN-001 Vision Node: what is verified before hardware (2026-11-24)

ADR 0020. Everything here is simulation or development-machine testing; no Zero 2 W, camera or
Frigate run has happened yet.

| Area | How | Result |
|---|---|---|
| Hub contracts, bookmark verification, media proxy, stream health | `bazel test //...`, plus `--config=asan` and `--config=ubsan` | 35/35 in each mode |
| Media proxy security | `//server:server_test` (`HttpE2E.Media*`) against a fake Frigate HTTP server | Only journaled ids, and only to humans with a `sensitive_read` grant; path-like ids never reach the recorder; the media index is rebuilt after a restart |
| Camera agent | `firmware/vision_node/tests`: 19 unit tests, plus `test_real_hub.py` against the real `talos_hub` binary | Healthy while streaming; DEGRADED ("camera stream down") when the stream stalls; FAULTED and the tamper alarm (patio speaker) when the lid opens |
| SD-card provisioning | `tests/test_provision.py` with a fake boot partition and a fake MediaMTX release | Secrets stay off the repository, the checksum is verified, the boot files can be rewritten safely, and bad input is refused |
| Frigate connector and config generator | `connectors/frigate` vitest (9) | Media refs, heartbeats only while frames arrive, bookmark to clip, failures ACKed as rejected, no secrets in the generated config |
| Twin | `vision_*` scenarios in the L1 and L2 corpora | Bookmark verified by its clip; with the stream down, no detections and the bookmark is never verified while the PIR warning still is; an unreachable recorder means no success; the lid opening raises the alarm |
| Website | `pnpm -C web e2e` (8/8), with devsim playing the camera and Frigate | Stream status, a snapshot fetched through the hub with the person's token, stream loss shown as DOWN, bookmark VERIFIED |
| Housing | `housing.py --check`: fit rules plus 13 solid/open probes | Passes. Thermal estimate: SoC about 64 °C by day and 74 °C at night with IR, at 35 °C ambient (estimate only) |

## Not yet run on this Mac

These need downloads, which are waiting on the owner's OK:
- the Frigate container (`ghcr.io/blakeblackshear/frigate:0.17.2`, about 1.5 GB);
- MediaMTX (darwin_arm64) for the virtual Vision Node.

With both, `deploy/frigate/README.md` and `firmware/vision_node/dev/run_virtual.sh` check four
things:
- that Frigate accepts the generated config;
- that a real Frigate event's media id is served through the hub;
- that a manual bookmark event produces a clip;
- that the connector's heartbeats follow the test-pattern stream.

## Needs the bench (VB-01..08)

| Bench test | What it checks |
|---|---|
| VB-01 | Stream stability and thermals on the Zero 2 W in the printed housing |
| VB-02 | Real PoE power |
| VB-03 | Cold boot time and survival of 200 power cuts |
| VB-04 | Night image through the window with the foam baffle |
| VB-05 | Pre-roll of a bookmark against the hub journal |
| VB-06 | Re-flash and recovery from a fresh card |
| VB-07 | NoIR daytime colour |
| VB-08 | The second (detection) stream |

Separately, the bought parts' dimensions in `housing.py` need checking against the real parts.
