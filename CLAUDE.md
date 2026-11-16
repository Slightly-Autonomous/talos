# Working on Talos

Read `docs/architecture/overview.md` and the ADRs in `docs/decisions/` before changing architecture. The original design handoff is `docs/spec/MASTER_SPEC_v0_2.md`; where it conflicts with an ADR, the ADR wins.

## Commands
- `bazel test //...`: all C++ tests (also `--config=asan`, `--config=ubsan`).
- `pnpm ts:check`: proto codegen, then typecheck/tests for every TS package, plus web lint/build.
- `bazel run //server:talos_hub -- --config $PWD/deploy/config/hub.dev.yaml`: local hub with dev tokens.
- `pnpm --filter @talos/devsim start` + `pnpm -C web dev`: website with virtual devices (token `dev-owner-token`).
- `pnpm -C web e2e`: Playwright E2E (needs `bazel build //server:talos_hub` first; boots hub + devsim + web).
- `pnpm proto:lint`: buf lint. CI also runs `buf breaking` against `main`.
- Format C++ with clang-format 21 (`.clang-format`). Format web with `pnpm -C web format`.

## Rules
- **Dependency boundaries:** `core/*` never includes protobuf, SQL, HTTP, simulation, drivers or UI. Protobuf mapping lives in `codec/`. `simulation` depends on `core`, never the reverse.
- **Determinism:** core never reads the wall clock or uses unseeded randomness. Inject `IClock`. Cross-device order comes from `journal_seq` only (ADR 0008).
- **Commands:** TTL gates dispatch only. Verification has its own window. Late evidence is recorded with `late=true`. Only `verified` is success (ADR 0004).
- **Ownership checks** belong in domain/ingest/policy, not the codec (ADR 0003).
- **Hardware is provisional:** never hard-code pins, rails, connectors or part choices in code. They belong in manifests marked `provisional: true`. Call out anything that needs datasheet or bench verification.
- **Scope:** build what the current phase needs, with tests. No speculative frameworks for later phases.
- **Core style:** exception-free (`Result`/`Status`), small value types, explicit units in names (`_ms`, `_mw`, `_ma`, `_milli_c`).
- **Before claiming done:** run the tests and report the exact command and result, plus what remains simulated or unverified. Update ADRs, `docs/decisions/open-decisions.md` and `docs/requirements.yaml` when architecture changes.
- Never commit secrets, keys, Wi-Fi/Thread credentials or real customer data. Configs hold token *hashes* only; cloud keys come from env vars.
- Supabase/Railway are config switches around the same hub and API (ADR 0010). Never make them the authority for alarm behavior.
- **Web UI:** a pure API client. Show missing/stale data as unknown, never clear; only `VERIFIED` renders as success; shade zones by region, never invent XY tracks (ADR 0011).
