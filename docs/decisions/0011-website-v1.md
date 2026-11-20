# ADR 0011: Website v1 (live, replay, engineering, simulation)

**Status:** Accepted (2026-11-16). Implements most of spec §28 and Phase 4. The editors are deferred to 4b.

## Decision
- **The website is a pure client of the hub API** through `@talos/contracts` `HubClient`. It never reads storage directly, and it keeps no business logic the hub does not also enforce. React plus TypeScript, with no router or UI framework.
- **Geometry is part of `talos.v1.Property`** (additive fields): zone polygons, walls, portal segments on assets, device positions and nominal coverage wedges. Core ignores geometry. Coverage is display metadata, not a calibrated sensing guarantee.
- **Live data:** load the property, the full journal (paged) and the state, then follow `/v1/events` over fetch streaming (the token stays in a header). After each new entry the client refetches `/v1/state`, and it also re-reads every 5 s because freshness depends on time. On error it reconnects with backoff and resumes from the last `journal_seq`.
- **Conservative presentation:** pure view-model functions in `web/src/model/view.ts`, unit-tested.
  - Zones are shaded by **region-level** evidence: active, recent, quiet or unknown. There are no fabricated XY tracks, because a PIR cannot say where in its zone someone is (spec §21). Every PIR in the zone counts, including declared ones that never reported: any stale or unknown PIR makes the zone unknown unless another is active, so "quiet" means every PIR is fresh and clear.
  - Command ids are random UUIDs (`ui-lock-<uuid>`); a 409 means another principal holds the id, so the UI retries once under a fresh id and otherwise reports a failure, never "sent".
  - Missing or stale data shows as **unknown**, never as clear or healthy.
  - Only `VERIFIED` renders as success. Unverified and unknown outcomes get distinct warning badges (invariant 6).
- **Replay** scrubs and plays the journal timeline. The state at each instant comes from the hub's `/v1/state_at`, which reduces the same journal the live engine uses. The heatmap counts motion detections per zone up to the scrub time.
- **Engineering view:** capability table with freshness, raw journal with source-order flags (gap, late, new boot) and a `clock_regressed` marker, plus per-entry JSON. Disclosure depth is a UI choice; it grants no extra authority.
- **Simulation:** `tools/devsim` runs virtual devices for the dev property as **real device-API clients**, each with its own device token. It covers a PIR, a contact, a speaker and a lock, with injectable faults (`silent_ack`, `no_response`, `jam`). A localhost-only control API drives it from the web Simulation panel. Everything flows through the same ingress, rules and policy as hardware.
- **Browser E2E:** Playwright boots the real hub binary on a fresh journal, plus devsim and the Vite app, and runs the exit scenario:
  1. Connect.
  2. Arm away.
  3. Trigger backyard motion.
  4. The zone shows active and the speaker command shows **Verified**.
  5. Replay start shows "Disarmed"; replay end shows the verified warning.

  The suite also covers "a silent-ACK speaker is never shown verified" and "a wrong token is refused".
- **Dev config:** the dev rule has no night-only schedule, so the demo and E2E work at any hour. The twin covers schedules.

## Deferred (Phase 4b)
- **Property editor:** floor-plan import, scale, drawing rooms and sensors. **Rule editor** with simulation preview. Both need versioned config-write APIs and journaled layout/rule revisions, so events keep referring to the layout that was in force when they happened (spec §21). That gets its own ADR.
- Camera clips and media references, multi-user roles, onboarding/pairing flows, and a PWA manifest.

## Known limits
- The token is held in memory only and is re-entered after a reload. A remembered session needs the auth work in the remote-access phase.
- The whole journal is loaded on connect. Windowed loading is needed for long histories.
