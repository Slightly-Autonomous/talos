# simulation

Deterministic digital twin (ADR 0009, spec §39-42). Devices run behind one seam (ADR 0018):
L1 behavioural models, or L2, the real portable firmware on the virtual board
(`talos_sim --backend firmware`). `backend_parity_test` holds L1 to L2 across the corpus.

- `base/`: portable PRNG and 2D geometry
- `twin/`: ground-truth world, virtual sensors and actuators (L1), the firmware backend (L2),
  virtual transport
- `runner/`: scenario runner, oracle, `talos_sim` CLI, corpus test
- `scenarios/`: sites and scenarios (see its README)

The twin drives the production `core/engine/HubEngine`, never a copy of it.
