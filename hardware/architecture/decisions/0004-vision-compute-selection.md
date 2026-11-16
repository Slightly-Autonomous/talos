# HW-0004: Vision Node compute — open, decided by an H5 benchmark

**Status:** Open (H5 not started).

## Decision so far
- The Vision Node is a separate IP device ([HW-0001](0001-hardware-family.md)).
- **Candidates:** Raspberry Pi CM4 (hardware H.264 1080p30 encode), CM5 (do **not** assume hardware video encode), and a dedicated video SoC.
- **The selection gate:** a camera benchmark covering encode format, resolution and frame rate; power; boot after power collapse; and CSI routing with the official carrier reference.
- **The manifest** `contracts/manifests/boards/vision_node_proto_a.yaml` describes zero sensor slots and power-source budgets only, and is marked provisional.

## Not decided
Compute module, camera optics (full Camera Module 3 board vs sensor assembly), PoE, battery/solar duty cycle and enclosure (HD-010..HD-014).
