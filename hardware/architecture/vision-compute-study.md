# TL-VN-001 Vision Node: compute and video desk study (H5, part 1)

**Status:** desk study, 2026-11-16; first build chosen 2026-11-24 (owner): **Raspberry Pi Zero 2 W + Camera Module 3 Wide NoIR + Waveshare PoE HAT** ([ADR 0020](../../docs/decisions/0020-vision-node-first-build.md)). The Zero 2 W was not in the original table: it has the CM4's class of H.264 hardware encoder (VideoCore IV) with no carrier board to design, at a fraction of the price. Nothing here is benchmarked; the bench gate below decides whether it stays (HD-010).

## Candidates

| Criterion | Raspberry Pi CM4 | Raspberry Pi CM5 | Rockchip RV1106 SoM class (e.g. Luckfox Core1106) |
|---|---|---|---|
| Video encode | **Hardware H.264 1080p30 encode** (CM4 datasheet/product brief) | **No hardware H.264 encoder.** libx264 software encode reaches 1080p30 real time using as little as about 60 % CPU (Raspberry Pi white paper RP-010033) | **Hardware H.264/H.265 encode**, reported up to 3072×1728 @ 30 fps. Some sources say 4 MP @ 15 fps for the RV1106B, so the exact part must be checked |
| Camera input | MIPI CSI-2, official camera stack (libcamera), Camera Module 3 supported | Same stack, two CSI ports on the module | 2 × 2-lane or 1 × 4-lane MIPI CSI-2 with an on-chip ISP. Sensor support depends on the vendor SDK |
| AI | CPU only (or an external accelerator) | Faster CPU; external accelerator over PCIe | 0.5 TOPS (G2) / 1 TOPS (G3) NPU on chip |
| Memory | 1–8 GB LPDDR4 | 2–16 GB | 128 MB (G2) / 256 MB (G3) DDR3L in package |
| Software | Mainline-friendly Raspberry Pi OS, mature libcamera | Same | Vendor Buildroot/SDK. Weaker mainline support: maintenance and OTA risk |
| Power (not sourced yet) | multi-watt class | higher than CM4 under encode load | expected sub-watt to ~1 W class, **unverified** |
| Lifecycle | product brief says production until at least Jan 2034 (dates differ between brief revisions) | long-term Raspberry Pi programme | depends on the SoM vendor |
| Fit for battery/solar duty cycle | marginal: Linux boot time and idle power | worst | best on paper |

## Reading
- **CM5 is the weakest fit for an encoder-centric camera.** It spends CPU and watts doing what the CM4 and RV1106 do in fixed-function hardware. Keep it only if on-device AI needs it.
- **CM4 is the low-risk default:** mature camera stack, hardware H.264, easy owner recovery (rpiboot) and self-flashing, which matches the open-source ownership goal (HD-016).
- **The RV1106 class is the power-optimised option** for battery/solar Vision Nodes. Its risks are the vendor SDK, image-sensor driver availability and long-term updates.

## Bench gate for the first build (VB-01..08; needs the bench kit)

| ID | Measure | Pass |
|---|---|---|
| VB-01 | 1080p15 H.264 to RTSP through MediaMTX for 24 h, inside the closed printed housing at 35 °C ambient: frame drops, `vcgencmd measure_temp`, `get_throttled` | no stall longer than 5 s; SoC below 80 °C; never throttled |
| VB-02 | PoE power at the switch port: idle, streaming by day, streaming with both IR boards lit | measured W per mode, within the 802.3af class the HAT negotiates; manifest figures replaced |
| VB-03 | Cold boot to the first RTSP frame, and 200 power cuts while streaming | under 30 s; no file-system damage (read-only overlay root) |
| VB-04 | Night: NoIR with the two 850 nm boards, a person at 5 m and at 8 m | identifiable in the Frigate clip; Frigate still detects a person |
| VB-05 | Pre-roll: a Talos PIR event, then a `camera.bookmark`; the clip includes 5 s before the event, aligned to the hub journal within 1 s | clip VERIFIED in Talos, pre-roll present |
| VB-06 | Owner re-flash and recovery from a fresh SD card with `tools/provision/vision_node.py` | documented and repeatable; no vendor keys |
| VB-07 | Daytime colour with the `imx708_wide_noir` tuning file | owner acceptable; otherwise move to an IR-cut camera |
| VB-08 | A second 640×360 stream for detection from MediaMTX on the Zero, alongside the main stream | both streams steady, or Frigate detects on the main stream |

The original CM4/CM5 benchmark ideas (24 h encode, power, cold boot, night, pre-trigger, recovery) are folded into VB-01..06.
