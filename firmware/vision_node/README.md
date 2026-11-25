# TL-VN-001 Vision Node software (ADR 0020)

The camera is a Raspberry Pi Zero 2 W with a Camera Module 3 Wide NoIR on a Waveshare PoE HAT. It
runs two services:

- **MediaMTX** (`system/mediamtx.yml.template`) serves the camera over RTSP.
  - Hardware H.264, 1080p at 15 fps, password-protected on the LAN.
  - It uses the NoIR tuning file.
  - An optional 640×360 detection stream is off until bench test VB-08.
- **The Talos agent** (`talos_vision/`, Python 3, standard library plus `python3-libgpiod`).
  - It reports this device (`cam_patio`) to the hub over the HTTP device API:
    - `device.health` heartbeats;
    - `camera.stream` status: whether video is flowing, plus resolution, frame rate, bitrate,
      SoC temperature and throttling;
    - `tamper.enclosure` from the lid microswitch on GPIO17.
  - Lid sensing runs every 20 ms with a 50 ms debounce, on its own loop. The network runs on
    another thread, so an unreachable hub never delays the lid. Reports wait in an ordered queue
    until the hub is back.

Frigate (`deploy/frigate/`) records and detects. Its view of the camera is a second device,
`cam_patio_video`, which the Frigate connector reports.

Pins and board facts are in `talos_vision/board.py`, which is checked against
`contracts/manifests/boards/vision_node_pi_zero2w.yaml`. All of them are provisional.

## Tests

```bash
cd firmware/vision_node && python3 -m unittest discover -s tests -t . -v
```

With the real hub binary (`bazel build //server:talos_hub`), `tests/test_real_hub.py` also runs the
agent against it. It checks three things: the camera is healthy while it streams, degraded when the
stream stalls, and an opened lid raises the tamper alarm.

```bash
TALOS_HUB=$PWD/bazel-bin/server/talos_hub bash -c 'cd firmware/vision_node && python3 -m unittest tests.test_real_hub -v'
```

## Virtual Vision Node (no camera)

Install the two tools once:

```bash
brew install mediamtx ffmpeg
```

Then start the virtual node. MediaMTX serves an ffmpeg test pattern at `rtsp://talos:dev-rtsp-password@127.0.0.1:8554/cam`,
and the agent runs with a fake lid (`/tmp/talos-vision/lid`; `echo 1 >` it to open the lid):

```bash
firmware/vision_node/dev/run_virtual.sh
```

Run it alongside the dev hub (`bazel run //server:talos_hub -- --config $PWD/deploy/config/hub.dev.yaml`)
and Frigate (`deploy/frigate/README.md`).

## A real camera

1. **Flash the card:** Raspberry Pi OS Lite, 64-bit, with Raspberry Pi Imager. Set a user and an SSH
   key, and leave Wi-Fi empty.
2. **Provision it:** `tools/provision/vision_node.py` (see its docstring). It prints the token hash for
   the hub config and the Frigate camera map entry, and shows the secrets once.
3. **First boot:** `system/firstrun.sh` installs everything, enables the watchdog and optionally the
   read-only root, then reboots.
4. **Run the bench gate:** VB-01..08 in `hardware/architecture/vision-compute-study.md`.

## Not covered yet

| Gap | Detail |
|---|---|
| Hardware | Nothing here has run on a Zero 2 W. Not yet checked: the MediaMTX keys on v1.21, the second stream, the libgpiod v1/v2 adapters, the first-boot script on Bookworm and Trixie images, and the Waveshare HAT's USB host-mode overlay. |
| Reset cause | The HTTP device API has no diagnostics channel, so the reset cause is not reported. Undervoltage history still reaches the hub in the `throttled` bits of `camera.stream`. |
| Secrets at rest | The Zero has no secure boot, so the card holds the token and RTSP password in clear (open-decisions.md). |
| Clock | NTP must sync before the agent reports. Without internet, use `--ntp <router or hub>`. |
