# HW-0004: Vision Node compute — open, decided by an H5 benchmark

**Status:** Narrowed (owner, 2026-11-24): the first build is a Raspberry Pi Zero 2 W with a Camera Module 3 Wide NoIR on PoE ([ADR 0020](../../../docs/decisions/0020-vision-node-first-build.md)). The bench gate VB-01..08 in `vision-compute-study.md` can still overturn it.

## Decision so far
- The Vision Node is a separate IP device ([HW-0001](0001-hardware-family.md)).
- **Candidates:** Raspberry Pi CM4 (hardware H.264 1080p30 encode), CM5 (do **not** assume hardware video encode), and a dedicated video SoC.
- **The selection gate:** a camera benchmark covering encode format, resolution and frame rate; power; boot after power collapse; and CSI routing with the official carrier reference.
- **The manifest** `contracts/manifests/boards/vision_node_pi_zero2w.yaml` (was `vision_node_proto_a`) describes zero sensor slots, the PoE and USB budgets and the lid tamper pin, all provisional.

## Not decided
Compute module, camera optics (full Camera Module 3 board vs sensor assembly), PoE, battery/solar duty cycle and enclosure (HD-010..HD-014).
