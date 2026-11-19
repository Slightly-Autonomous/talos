# ADR 0018: Twin backend seam and L1/L2 parity

**Status:** Accepted (2026-11-19). Implements bench-test-plan §3 layer 3.

## Context
The L1 twin (ADR 0009) models each device's behaviour directly. The portable firmware (ADR 0013) runs on a register-level virtual board. Until now the two never met, so the scenario corpus could claim behaviour the real firmware does not have (debounce, warm-up masks, retries, one impact threshold per node) and nobody would notice.

## Decision
- **One seam.** The runner talks to every device through `simulation/twin/backend.h` `IDeviceBackend`:
  - **L1** `VirtualDevice`: the behavioural model.
  - **L2** `FirmwareDevice`: the real `NodeRuntime` on `fw::sim::Node`, fed from the same world. Contacts and the PIR output are GPIO levels. Impacts are LIS2DW12 accelerations. Zone temperature goes into the SHT40. The PIR's optics (wedge, walls, pets, misses, hold) are shared with L1 as `PirOptics`; everything after the sensor pin is the firmware's. Envelopes come from the same `firmware/bridge` mapping the hub's CoAP ingress uses.
  - **L4** (hardware): declared only. A device bound to it makes the scenario **skipped**, never passed.
- **Binding.** `SiteDevice.binding` picks a backend per device. `UNSPECIFIED` follows the run's `--backend` (`talos_sim --backend firmware`): every device the firmware can play goes on L2, the rest on L1. `FirmwareDevice::unsupported_reason` says why a device cannot run on L2: actuators, a camera, an unknown module, or two impact thresholds on one node. An explicit binding the backend cannot honour skips the scenario with that reason. The report lists each device's backend.
- **ACKs come from the hub.** An L2 uplink is ACKed only when the hub takes it, so an uplink lost in the twin's transport is retried by the firmware's own queue. Diagnostics with no envelope (module present, config applied) are ACKed at once, as the CoAP ingress does.
- **Start versus reboot.** At scenario start every device is already installed and past warm-up. L2 pre-runs the firmware for its PIR mask (31 s) and delivers the latest state reports at t=0. A `device_reboot` stimulus is a real power-on: L2 is PIR-blind for 30 s, and L1 for `PirSpec.warmup_ms`.
- **Parity.** `//simulation/runner:backend_parity_test` runs every scenario on L1 and on L2. It checks:
  - Both runs pass.
  - Every L2 device journals the same number of each transition as its L1 model: `motion.detected/cleared`, `contact.opened/closed`, `impact.detected`. Heartbeats, boot reports and environment readings follow each backend's own schedule; the scenarios' expectations cover them.
  - At least 20 scenarios actually run firmware.

## Consequences
- 29 of 29 scenarios run with at least one firmware device. All pass on both backends with matching transitions. Mutating the L1 impact threshold, or dropping impacts on L2, makes the parity test fail.
- Building parity found one real difference: the firmware masks the PIR for 30 s after power-on, while L1 detected immediately. The start/reboot rule above makes this explicit. After a reboot, the twin now shows the blind window the hardware will have.
- The twin now depends on `firmware/sim` and `firmware/bridge`. `core` still depends on neither.
- Not modelled on L2: downlink latency for ACKs and config (they arrive on the next firmware step), and SHT40 measurement noise.
