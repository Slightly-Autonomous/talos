# Talos

An open-source, **local-first spatial security and automation platform** for houses and other properties.

- Modular battery/solar sensor nodes: an ESP32-H2-class controller with four coplanar sensor-module slots.
- A local hub that owns all events, state, rules and commands, and works without internet.
- A deterministic digital twin that runs the real production logic in CI.
- A website (PWA first). Optional cloud and observability are never in the alarm path.

> **Status: Phase 4 (website v1).** Contracts, domain core, an L1 twin (29 scenarios), `talos_hub` (SQLite journal, HTTP API with live events, token auth, optional Supabase copy, Docker/Railway deploys), and a website with live floor plan, replay, engineering and simulation views backed by browser E2E. Property/rule editors and hardware are not built yet. See [roadmap](#roadmap). Everything is experimental and is not a certified alarm or life-safety system.

## Quick start

Prerequisites: [bazelisk](https://github.com/bazelbuild/bazelisk), a C++20 compiler (Apple clang 17 / GCC 13+), Node ≥ 20.19 and pnpm 10.

```bash
bazel test //...                    # all C++ tests
```

```bash
bazel test --config=asan //...      # under AddressSanitizer (also: --config=ubsan)
```

```bash
pnpm install && pnpm ts:check       # proto codegen + all TS typecheck/tests + web lint/build
```

```bash
pnpm proto:lint                     # buf lint on contracts/proto
```

```bash
bazel run //server:talos_hub -- --config $PWD/deploy/config/hub.dev.yaml   # then: curl localhost:8080/v1/health
```

See [`deploy/README.md`](deploy/README.md) for Docker Compose, Supabase and Railway.

Run the website against that hub, with virtual devices (log in with `dev-owner-token`):

```bash
pnpm --filter @talos/devsim start
```

```bash
pnpm -C web dev                     # http://localhost:5173
```

```bash
pnpm -C web e2e                     # browser E2E: boots hub + devsim + web itself
```

```bash
bazel run //simulation/runner:talos_sim -- $PWD/simulation/scenarios/scenarios/backyard_intrusion_at_2am.yaml --root $PWD/simulation/scenarios
```

## Repository map
| Path | What |
|---|---|
| `contracts/proto/talos/v1` | Protobuf contracts (the wire/storage schema) |
| `contracts/manifests` | Sample board/module/property YAML. **All numbers provisional** |
| `core/` | Pure C++20 domain core: journal, ingest/dedup, state/replay, commands, policy, rules, manifest validator |
| `codec/` | Proto ↔ core mapping and the YAML loader |
| `simulation/` | Digital twin: world, virtual devices/transport, scenario runner, scenario corpus |
| `server/` | `talos_hub`: SQLite journal, HTTP API v1 + SSE, auth, config switches |
| `packages/contracts` | Generated TypeScript contracts and typed `HubClient` |
| `connectors/supabase_sync` | Optional read-only Supabase copy |
| `deploy/` | Dev config, Docker Compose, Railway templates, Supabase migrations |
| `web/` | React + TypeScript website: live floor plan, replay, engineering, simulation (Playwright E2E) |
| `tools/devsim` | Virtual devices for the dev property, over the hub's device API |
| `docs/` | Architecture, ADRs, open decisions, security invariants, requirements, original spec |
| `firmware/ hardware/ observability/` | Placeholders for later phases |

Start with [`docs/architecture/overview.md`](docs/architecture/overview.md) and [`docs/decisions/`](docs/decisions/README.md).

## Roadmap
0–1. Repo, contracts, deterministic core
2. Deterministic L1 digital twin and scenario runner
3. Local hub: SQLite journal, API, auth
4. Website: live state, replay ← *here* (4b: property and rule editors)
5. Portable firmware on virtual hardware, then ESP-IDF
6. Thread/CoAP, OTA, observability, hardening
7. Optional Supabase/Railway, MCP connector
8. Dev-board validation, custom PCB, manufacturing

## License
Apache-2.0. See [LICENSE](LICENSE).
