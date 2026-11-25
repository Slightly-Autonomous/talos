# ADR 0020: Vision Node first build: Pi Zero 2 W, Camera Module 3 Wide NoIR, PoE, Frigate

**Status:** Accepted (owner, 2026-11-24). Narrows [HW-0004](../../hardware/architecture/decisions/0004-vision-compute-selection.md)
for the first build; the bench benchmark (VB-01..08) can still overturn it.

## Context
HW-0001 made the camera a separate IP device (TL-VN-001): video never goes through a Sensor Node slot
or over Thread. HW-0004 left the compute open (CM4, CM5, a video SoC). The owner wants Talos's own
camera ready to order together with the TL-SN-002 boards and the bench kit, and chose (2026-11-24):
- **Option B:** a Raspberry Pi Zero 2 W with a Raspberry Pi **Camera Module 3 Wide NoIR** (IMX708,
  120°, autofocus, HDR), plus 850 nm IR light for night.
- **Bought parts only** for the first build, no custom PCB. The IR boards switch themselves with
  their own photoresistors; the lid tamper is a microswitch on a GPIO.
- **Frigate runs on the development Mac for now** (Docker, CPU detector). The production recorder
  hardware is decided later.

## Decision
1. **Hardware (manifest `contracts/manifests/boards/vision_node_pi_zero2w.yaml`, provisional):**
   - Zero 2 W: hardware H.264 encoding, mature libcamera stack, owner-reflashable.
   - Waveshare PoE/ETH/USB HUB HAT: 802.3af into an isolated 5 V supply, plus 100 Mbit Ethernet.
   - Ethernet only. Wi-Fi and Bluetooth are off, so the camera's 2.4 GHz traffic never competes
     with Thread (spec §33).
   - Rejected for now:
     - CM5: no hardware H.264 encoder.
     - CM4: needs a carrier board; it comes back if a custom carrier is ever built.
     - RV1106: vendor SDK and update risk.
2. **Video path:**
   - On the camera, MediaMTX's `rpiCamera` source does the hardware H.264 encoding and serves RTSP with a password on the LAN.
   - Frigate pulls the stream, records with pre-capture, and runs person, vehicle and animal detection.
   - Video bytes stay in Frigate (spec §33, ADR 0015). The hub journals metadata and media references only.
3. **Two linked devices per camera.** The hub accepts one source-position stream per device id
   (ADR 0008), so two producers on one id would suppress each other:
   - `<cam>`, the **Vision Node agent** on the Pi. It reports `device.health` heartbeats,
     `camera.stream` status and `tamper.enclosure`, and handles maintenance.
   - `<cam>_video`, the **Frigate connector**. It reports `camera.detection` with media references,
     and executes `camera.recording` bookmarks.
   - `Device.host_device_id` links the second to the first, so the UI shows one camera.
4. **Bookmarks are verified commands.** A rule (say, motion in the backyard while armed) sends
   `camera.bookmark` to `camera.recording`. The connector creates a Frigate event, and the command
   is VERIFIED only when a `ClipRecorded` observation with that command id arrives. An unreachable
   Frigate gives `unknown_result`, never success (ADR 0004).
5. **Media goes through the hub, with checks.** `GET /v1/media/...` serves a snapshot or clip only to
   a principal with media access, and only for a media id that this device's journaled observations
   referenced. The hub is therefore not a general proxy into Frigate.
6. **Clock and identity on a board with no RTC:**
   - HTTP boot epochs are Unix seconds (ADR 0008/0014). The agent waits for NTP, then uses
     `max(now, last + 1)`, persisted on a writable data partition.
   - Identity (device id, token, hub URL, RTSP password) is written to the SD card at provisioning
     and never committed.
7. **Robustness:**
   - Read-only overlay root plus a small data partition, to survive power cuts.
   - The hardware watchdog is on.
   - Each boot reports its reset cause: power-on, watchdog, or undervoltage seen from `vcgencmd get_throttled`.

## Consequences
- New contract pieces:
  - payloads `StreamStatus`, `ClipRecorded` and `MediaRef`;
  - `ObjectDetected.media`;
  - `Device.host_device_id`;
  - operation `camera.bookmark`;
  - power source `POWER_SOURCE_POE`;
  - capabilities `camera.stream` and `camera.recording`.
- **Residual risk:** the Zero has no secure boot or flash encryption, so a stolen SD card exposes that
  camera's device token and RTSP password. The token's scope is that one device (ownership checks,
  ADR 0003). The owner can revoke it, and opening the lid raises tamper (open-decisions.md).
- **Night image:** the NoIR sensor has no IR-cut filter, so daytime colour is shifted. The
  `imx708_wide_noir` tuning corrects part of it. A motorised IR-cut camera is the fallback if VB-04/07 fail.
- **Pi Zero 2 W limits:** 512 MB, thermal throttling in a closed housing, and stream stalls
  reported by some users. These are measured on the bench (VB-01, VB-08) before more cameras are
  built.

## Bench gate (replaces the VB table in `hardware/architecture/vision-compute-study.md`)
See that study for VB-01..08.
