# HW-0001: Hardware family — bought hub, one Sensor Node, separate Vision Node

**Status:** Accepted (2026-11-16). Supersedes the camera *slot* module (`contracts/manifests/modules/smart_camera_s3_v1.yaml`) and the fifth "power cartridge" idea from the master spec.

## Decision
- **TL-HUB-REF is bought, not designed.** A Pi 5 or mini-PC with a USB 802.15.4 RCP, normal mains power and an optional UPS.
- **TL-SN-001 is the one bespoke main board** in the first batch. It has four coplanar sensor slots, Thread, and integrated USB-C/PV/battery power.
- **TL-VN-001 is a separate board** with its own compute, a direct CSI camera, IP transport and its own power path. Video never travels through a Sensor Node slot: the bandwidth, power and thermal needs are a different product class.
- **Cards** TL-MOD-001..004 and the test cartridge TL-TEST-001 share one connector contract ([HW-0002](0002-sensor-module-interface.md)).

## Consequences
- `smart_camera_s3_v1` is kept only so the legacy validator tests keep running. It is marked superseded, and new boards do not route a camera to a slot.
- The domain model stays general: board types are data (`BoardManifest`), and a board with zero slots is valid (the Vision Node).
