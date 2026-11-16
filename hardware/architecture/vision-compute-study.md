# TL-VN-001 Vision Node: compute and video desk study (H5, part 1)

**Status:** desk study only, 2026-11-16. Nothing here is benchmarked. It narrows the candidates and defines the benchmark that decides HD-010 ([HW-0004](decisions/0004-vision-compute-selection.md)).

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

## Benchmark that decides HD-010 (needs hardware and owner approval to buy dev kits)

| ID | Measure | Pass |
|---|---|---|
| VB-01 | 1080p30 H.264 encode to RTSP for 24 h, CPU and SoC temperature | no dropped frames in a 45 °C enclosure |
| VB-02 | Power: idle, streaming, event-clip only | measured W per mode, fed into `power_budget.py` |
| VB-03 | Cold boot to first frame, after power collapse | < 20 s, no file-system corruption over 200 power cuts |
| VB-04 | Night: NoIR + 850 nm IR, person at 8 m | identifiable in clip |
| VB-05 | Pre-trigger ring buffer, 5 s before a Talos sensor event | clip includes the pre-roll, with a timestamp aligned to the hub journal |
| VB-06 | Owner re-flash and recovery without vendor keys | documented and repeatable |

The schematic work (H6) starts only after VB-01..06 pick the platform.
