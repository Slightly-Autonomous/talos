# ADR 0009: L1 digital twin and scenario runner

**Status:** Accepted (2026-11-16). Implements spec §39-42 and Phase 2.

## Decision
- **One production engine.** `core/engine/HubEngine` is the real hub loop: ingest → reduce → verify → rules → policy → ledger → outbound. It replaced the test-only `MiniHub`. The twin drives this exact class, and the Phase 3 server will wrap it. There is no simulated variant of business logic.
- **Twin lives in `simulation/`** and depends on core, never the reverse:
  - `base`: PRNG and geometry.
  - `twin`: world, virtual devices and transport.
  - `runner`: scenario runner, oracle, CLI and the corpus test.
- **Fixed-step virtual time**, 100 ms per step. In each step:
  1. Apply stimuli.
  2. Move actors.
  3. Sample sensors.
  4. Deliver due messages.
  5. Run engine `tick()`.
  6. Send outbound commands.
- **Portable determinism.** Randomness comes from SplitMix64 with named sub-streams (`Rng::stream(seed, name)`), never `std::` distributions, which differ between libc++ and libstdc++. Geometry uses IEEE-exact arithmetic, and trig runs only once, at setup. Every scenario runs twice in the test and must produce byte-identical journal digests.
- **Ground truth vs belief.** `World` holds actor positions, portal state and actuator effects. The engine only receives `ObservationEnvelope`s and ACKs. The oracle compares the two. These invariants always run:
  - A **verified** actuator command must have a recorded physical effect.
  - No command acts more than once.
  - Replaying the journal reproduces the engine state.
- **Closed loop.** The virtual speaker, lock and notifier execute delivered commands, change the world, and report back over the same transport. Devices deduplicate commands by command id. Actuator faults are `silent_ack`, `no_response` and `jam`.
- **Virtual transport** has per-device latency, jitter, loss, duplication and disconnects, all seeded. Reordering emerges from jitter.
- **Fixtures** are YAML instances of `talos.sim.v1.Site` and `talos.sim.v1.Scenario`, loaded with the existing YAML loader. Rules use the production `talos.v1.Rule` message.
- **Failure output:** seed, failure list, a `<name>.trace.json` file (ground truth plus the journal as protobuf JSON) in Bazel's undeclared outputs, and a rerun command for `talos_sim`. CI uploads these traces when a run fails.

## Fidelity limits (deliberate)
- L1 behavioral models only. There is no contact bounce or debounce, GPIO, sleep or energy model. Those come with the portable firmware and virtual board in Phase 5, so firmware logic is never duplicated in the twin.
- Actors follow scripted paths through walls. Walls only block sensor line of sight.
- PIR fires on moving, visible actors inside a wedge. It models a hold time, miss probability, pet immunity and false triggers. Camera output is detection metadata only, never pixels.
- Devices drop messages while disconnected. Store-and-forward and retry belong to firmware (Phase 5), and real Thread behavior to OTNS (Phase 6).
- Time zones use the site's fixed UTC offset, with no DST.
- All timing and probability values are placeholders, not calibrated figures (spec §42).

## Core additions made for this phase
- `ObjectDetected` and `Heartbeat` payloads.
- `ArmModeChanged` journal event, with arm mode now part of `PropertyState`.
- `authorize_arm_change`: disarming requires a human with a security_critical grant.
- The engine evaluates rules only when an observation becomes current state, so a late, superseded event is history, not a trigger.
