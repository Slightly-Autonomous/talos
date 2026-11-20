# ADR 0010: Hub server, storage, API and deployment switches

**Status:** Accepted (2026-11-16). Implements spec §28-29 and Phase 3.

## Decision
- **One server binary** (`//server:talos_hub`) wraps the production `HubEngine` behind a thread-safe `HubService`. A single mutex serializes engine calls, so the engine stays single-threaded and deterministic given its inputs. A tick thread closes TTLs and verification windows. The wall clock (`SystemClock`) lives in `server/`, never in core.
- **Storage:** `SqliteJournal` implements `IJournal`.
  - It runs in WAL mode with `synchronous=FULL`, so an acknowledged append survives power loss.
  - Each row stores the serialized `talos.v1.JournalEntry`, with `journal_seq` as the primary key.
  - `open()` fails closed on a failed `quick_check`, an undecodable row, a sequence gap or a newer schema.
  - Both journals pass the same conformance suite (`//core/journal:conformance`).
  - The engine rebuilds its state from the journal on start.
- **API v1** is JSON over HTTP using the protobuf JSON mapping (`talos.api.v1`), plus Server-Sent Events (`/v1/events`) for live journal entries. TypeScript types and a typed `HubClient` live in `packages/contracts`. A local hub and a cloud-hosted hub serve the identical contract, so clients change only the base URL.
  - **Public:** `GET /v1/health`.
  - **Principals (people/agents):** `/v1/property`, `/v1/state`, `/v1/state_at`, `/v1/journal` (paged; also the sync outbox), `/v1/events`, `POST /v1/arm`, `POST /v1/commands` (202 + status), `GET /v1/commands/{id}`.
  - **Device credentials:** `POST /v1/devices/observations`, `POST /v1/devices/acks`, `GET /v1/devices/commands`.
- **Authentication (local mode):** bearer tokens.
  - Configs store only SHA-256 hashes. SHA-256 is a small FIPS 180-4 implementation checked against NIST vectors. BoringSSL was dropped because its headers collide with system OpenSSL headers on common developer machines.
  - A device credential may only report for its own device id, and may only ACK commands targeting it.
  - Principals cannot use device ingress, and devices cannot use the user API.
  - Authorization stays in `core/policy`. Policy denials surface as 403 for arming, or as a recorded `denied` command for command requests.
- **Tokens never go in URLs.** SSE clients use fetch streaming with the Authorization header, not `EventSource`.
- **Configuration switches** (`talos.server.v1.HubConfig`, YAML):
  - `storage.driver`: `SQLITE` (implemented) | `POSTGRES` (reserved).
  - `sync.target`: `NONE` | `SUPABASE` (implemented through the out-of-process `connectors/supabase_sync` worker).
  - `auth`: `LOCAL` (implemented) | `SUPABASE` (reserved).
  - `role`: `HUB` (implemented) | `RELAY` (reserved).
  - Reserved values fail at startup with `reserved_option`. There are no silent fallbacks.
- **Supabase is a copy, not the authority.** The sync worker reads the hub API with a read-only token and upserts journal, capability state, commands and property state into Supabase. Every write is keyed by stable ids, so the worker is idempotent, and it resumes from a cursor. Tables use row-level security (members read their own properties), and only the service role writes. Supabase never issues commands.
- **Railway is a deploy target, not a code path.** It runs the same containers (`server/Dockerfile`, `connectors/supabase_sync/Dockerfile`) through config-as-code files in `deploy/railway/`. Running the hub on Railway makes that property's authority cloud-hosted; this is documented as a demo/simulation option, not the recommendation for a real home.
- **Dev-token guard:** configs marked `dev_mode: true`, and any config that carries a published dev token hash or dev device key (with or without `dev_mode`), refuse to listen on non-loopback addresses unless `TALOS_ALLOW_DEV_ON_NETWORK=1`, which only the localhost-bound Compose file sets.
- **Connection guard:** each HTTP connection must deliver a request's headers and body within 10 s (a watchdog shuts the socket down; handlers clear the deadline while they run, and SSE streams keep it cleared), and one peer address may hold at most 32 connections. The worker pool is 16 base, 128 maximum. cpp-httplib is patched in `MODULE.bazel` (`patch_cmds`) only to make `Server::process_and_close_socket` overridable. Behind the TLS proxy every connection comes from the proxy, so the proxy must enforce its own header and body timeouts and the per-peer cap should be raised there.
- **Ingress transport:** cpp-httplib is built as plain HTTP. TLS is terminated in front of the hub, by a reverse proxy or the Railway edge.

## Deferred / known limits
- The Postgres storage driver needs libpq. The interface and conformance suite are ready for it.
- The relay role belongs to the remote-access phase. Supabase JWT auth is implemented (ADR 0015).
- There is no per-principal *read* scoping yet: any authenticated principal can read state and journal. Read scopes arrive with multi-user support (spec §28 story 9).
- Accepted commands wait in the engine until the target's adapter polls. The poll is the dispatch (ADR 0004). A command whose TTL passes first becomes `expired` and is never delivered. Pending commands survive a restart because they are rebuilt from the journal.
- Command requests are rate limited per principal (ADR 0004 amendment); other routes rely on the 64 KB body cap and the connection guard. Firmware imports allow 4 MB bodies, read before authentication (bounded by the read deadline). Live `/v1/events` streams are capped at 16 concurrent, at most 4 per principal, with the last 4 slots reserved for owners (503 beyond that), so streams cannot starve device ingest of HTTP workers or lock the owner out. `/v1/state` still returns every non-denied command; pagination is future work. Full-journal replays (`/v1/state_at`, `/v1/config?at=`, `/v1/rules/preview`) copy entries under the hub mutex and reduce outside it, but are still O(history). Snapshots are future work.
- Time zones use a fixed UTC offset (no DST).
