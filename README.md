# Talos

An open-source, **local-first spatial security and automation platform** for houses and other properties.

- Modular battery/solar sensor nodes: an ESP32-H2-class controller with four coplanar sensor-module slots.
- A local hub that owns all events, state, rules and commands, and works without internet.
- A deterministic digital twin that runs the real production logic in CI.
- A website (PWA first). Optional cloud and observability are never in the alarm path.

> **Status: Phase 1 (contracts + pure domain core).** No hardware, server, twin or UI yet. See [roadmap](#roadmap). Everything is experimental and is not a certified alarm or life-safety system.

## Quick start

Prerequisites: [bazelisk](https://github.com/bazelbuild/bazelisk), a C++20 compiler (Apple clang 17 / GCC 13+), Node ≥ 20.19 and pnpm 10.

```bash
bazel test //...                    # all C++ tests
```

```bash
bazel test --config=asan //...      # under AddressSanitizer (also: --config=ubsan)
```

```bash
pnpm install && pnpm web:check      # proto codegen + web lint/typecheck/test/build
```

```bash
pnpm proto:lint                     # buf lint on contracts/proto
```

## Repository map
| Path | What |
|---|---|
| `contracts/proto/talos/v1` | Protobuf contracts (the wire/storage schema) |
| `contracts/manifests` | Sample board/module/property YAML. **All numbers provisional** |
| `core/` | Pure C++20 domain core: journal, ingest/dedup, state/replay, commands, policy, rules, manifest validator |
| `codec/` | Proto ↔ core mapping and the YAML loader |
| `web/` | React + TypeScript client (shell only for now) |
| `docs/` | Architecture, ADRs, open decisions, security invariants, requirements, original spec |
| `simulation/ server/ firmware/ hardware/ …` | Placeholders for later phases |

Start with [`docs/architecture/overview.md`](docs/architecture/overview.md) and [`docs/decisions/`](docs/decisions/README.md).

## Roadmap
0–1. Repo, contracts, deterministic core ← *here*
2. Deterministic L1 digital twin and scenario runner
3. Local hub: SQLite journal, API, auth
4. Website: property editor, live state, replay
5. Portable firmware on virtual hardware, then ESP-IDF
6. Thread/CoAP, OTA, observability, hardening
7. Optional Supabase/Railway, MCP connector
8. Dev-board validation, custom PCB, manufacturing

## License
Apache-2.0. See [LICENSE](LICENSE).
