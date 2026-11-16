# Architecture overview

Talos is a local-first spatial security and automation platform. Full background is in [`docs/spec/MASTER_SPEC_v0_2.md`](../spec/MASTER_SPEC_v0_2.md). Decisions are in [`docs/decisions/`](../decisions/README.md).

## Authority
```mermaid
flowchart TB
  W[Website / future mobile / AI connectors] -->|intents via API| H
  subgraph H[Local hub: authoritative]
    I[Ingest + dedup] --> J[(Journal: journal_seq)]
    J --> S[State projection]
    S --> R[Rules] --> P[Policy] --> L[Command ledger]
    L --> J
  end
  N[Thread nodes / IP devices / digital twin] --> I
  L -->|dispatch| N
  H -.optional.-> C[Cloud projection / Grafana]
```
The hub owns events, state, rules, commands and audit. Everything else is either a client or an optional, non-critical projection.

## Packages and dependency rules
| Package | Contains | May depend on |
|---|---|---|
| `core/base` | `Result`, `Status` | — |
| `core/time` | `Timestamp`, `IClock`, `VirtualClock` | — |
| `core/domain` | IDs, property/zone/asset/device, ownership validation | base |
| `core/events` | `ObservationEnvelope`, typed payloads, validation | domain, time |
| `core/execution` | `Command`, lifecycle, `CommandLedger` | events |
| `core/journal` | `JournalEntry`, `IJournal`, `InMemoryJournal` | events, execution |
| `core/ingest` | `SourceTracker` (bounded dedup), `Ingestor` | journal, domain |
| `core/state` | pure reducer, replay, `state_at`, freshness | journal, execution |
| `core/policy` | grants, risk classes, `evaluate` | execution |
| `core/rules` | rules → intents → commands | domain, events, execution |
| `core/manifest` | board/module manifests, slot/power validator | domain |
| `codec` | proto ↔ core mapping, YAML loader | core, generated protos |
| `contracts/proto` | `talos.v1` schemas | — |
| `web` | React client | generated TS only |

**Hard rule:** `core` never depends on protobuf, SQL, HTTP, simulation, drivers or UI. `simulation` (Phase 2) depends on `core`, never the reverse.

## Data planes (spec §27)
- **Control:** command transitions are journaled. Highest priority.
- **Domain:** observations are journaled and projected to state and replay.
- **Observability:** diagnostics are kept apart from the journal and dropped first. Lands in Phase 6.

## What is real vs provisional
- **Real and tested:** everything under `core/`, `codec/` and `contracts/`, plus the web shell.
- **Provisional data:** every number in `contracts/manifests/`.
- **Not started:** twin, server, UI, firmware, networking, OTA, cloud, hardware.
