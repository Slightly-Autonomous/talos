# simulation

L1 deterministic digital twin (ADR 0009, spec §39-42).

- `base/`: portable PRNG and 2D geometry
- `twin/`: ground-truth world, virtual sensors and actuators, virtual transport
- `runner/`: scenario runner, oracle, `talos_sim` CLI, corpus test
- `scenarios/`: sites and scenarios (see its README)

The twin drives the production `core/engine/HubEngine`, never a copy of it.
