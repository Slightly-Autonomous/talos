# TL-VN-001 Vision Node: what is verified before hardware (2026-11-24)

ADR 0020. Everything here is simulation or development-machine testing; no Zero 2 W or camera has
run yet. Real Frigate and MediaMTX ran on this Mac on 2026-11-25 (below).

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

## Real Frigate and MediaMTX on this Mac (2026-11-25)

Frigate 0.17.2 in Docker (CPU detector), MediaMTX 1.21.4, the virtual Vision Node
(`dev/run_virtual.sh`: a 1280×720 15 fps test pattern published from a container, because this Mac's
Homebrew ffmpeg is broken), the dev hub and the Frigate connector.

| Check | Result |
|---|---|
| Frigate accepts the generated config | Yes; decodes `patio` at 5 fps. Only warnings: no hardware decoding, CPU detector |
| Agent and connector reach the hub | `cam_patio` and `cam_patio_video` both HEALTHY |
| Bookmark (10 s) through Frigate | VERIFIED 25 s after the request; the clip from the hub's media proxy is a 15.0 s MP4 (5 s pre-roll + 10 s) |
| Media proxy | Snapshot (JPEG) and clip (MP4) served to the owner; a device token gets 403; an unjournaled id gets 404 |
| Stream cut (publisher stopped) | `cam_patio` DEGRADED "camera stream down" within 10 s; Frigate drops to 0 fps and the connector stops `cam_patio_video` heartbeats. That device only reads stale after the hub's freshness age (15 min in the dev config) |
| Stream back | `cam_patio` HEALTHY within 20 s; Frigate reconnects by itself |
| Lid opened (fake lid file) | `cam_patio` FAULTED "enclosure tamper", and the `enclosure_tamper_alarm` rule dispatches |

**Bug found and fixed:** Frigate gives a timed (bookmark) event its `end_time` and `has_clip` when
the event is created, so the connector reported the clip, and the hub VERIFIED the bookmark, about
4 s after the request, before the clip was recorded. The connector now waits until `end_time` plus
12 s (Frigate writes 10 s segments) has passed, and the unit test's fake Frigate behaves like the
real one.

Not covered: a real person/vehicle detection (the test pattern has none; detections with media refs
are covered by the connector tests and the web E2E), and anything on Pi hardware.

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
