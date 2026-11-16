# Scenario corpus

Deterministic digital-twin fixtures (spec §42). `sites/` holds geometry and device placements (`talos.sim.v1.Site`). `scenarios/` holds stimuli and expected outcomes (`talos.sim.v1.Scenario`). Both are YAML instances of the protos in `contracts/proto/talos/sim/v1/`.

- Every scenario runs through the **real** `HubEngine`, twice. It must pass, and both runs must produce identical journals.
- Always-on invariants on top of each file's expectations:
  - a verified command must have physically happened in the world
  - no command acts more than once
  - replaying the journal reproduces the engine state
- On failure the test writes `<name>.trace.json` (seed, ground truth, journal) to Bazel's undeclared outputs and prints a rerun command.

Run one scenario:

```bash
bazel run //simulation/runner:talos_sim -- $PWD/simulation/scenarios/scenarios/backyard_intrusion_at_2am.yaml --root $PWD/simulation/scenarios --trace /tmp/trace.json
```

Timing values are behavioral placeholders, not calibrated firmware or radio figures (spec §42).
