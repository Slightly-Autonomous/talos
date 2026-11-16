# Hub HTTP API v1

The API is JSON over HTTP using the protobuf JSON mapping. The schemas are in `contracts/proto/talos/api/v1/api.proto`, and a typed client is in `packages/contracts/src/client.ts`. A local hub and a cloud-hosted hub serve the same contract (ADR 0010).

**Auth:** `Authorization: Bearer <token>`.
- **Principals** (people and agents) use local tokens, or Supabase JWTs in `AUTH_MODE_SUPABASE` (ADR 0015).
- **Devices** use device tokens for the HTTP device API.
- Tokens never go in URLs.

## Public
| Method | Path | Notes |
|---|---|---|
| GET | `/v1/health` | Liveness, journal seq, property id |

## Principals
| Method | Path | Notes |
|---|---|---|
| GET | `/v1/property` | Current layout (`talos.v1.Property`) |
| GET | `/v1/state` | Capabilities with freshness, arm mode, commands |
| GET | `/v1/state_at?t=<unix_ms>` | Journal-prefix state at a past instant |
| GET | `/v1/journal?after=&limit=` | Paged journal (also the sync outbox); max 500 per page |
| GET | `/v1/events?after=` | Server-Sent Events of new journal entries (use fetch streaming) |
| POST | `/v1/arm` | `{mode}`. Disarm: human with security-critical grant |
| POST | `/v1/commands` | `CommandRequest` → 202 with status (denied/dispatched/…) |
| GET | `/v1/commands/{id}` | Lifecycle, outcome (only `VERIFIED` is success), history |
| GET | `/v1/config[?at=]` | Versioned layout and rules (ADR 0012) |
| PUT | `/v1/config` | `{baseRevision, document}`; owner only; 409 on conflict |
| POST | `/v1/rules/preview` | Dry-run a rule against history; dispatches nothing |
| GET | `/v1/health/devices` | Health projection per device (ADR 0014) |
| GET | `/v1/diagnostics?device_id=` | Recent device diagnostics (never the journal) |
| POST | `/v1/devices/{id}/config` | Desired node config; owner only |
| POST | `/v1/firmware/releases` | `{manifest, image}`; Ed25519 signature from an owner key; owner only |
| GET | `/v1/firmware/releases` | Verified releases (signatures omitted) |
| POST | `/v1/firmware/campaigns` | `{deviceId, version}`; owner only |
| GET | `/v1/firmware/campaigns/{device}` | Campaign state |
| GET | `/metrics` | Prometheus text, bounded labels |

## Devices (HTTP device API)
| Method | Path | Notes |
|---|---|---|
| POST | `/v1/devices/observations` | A device may only report for itself |
| POST | `/v1/devices/acks` | Only the command's target may ACK |
| GET | `/v1/devices/commands` | Drains pending commands for this device |

## Devices (CoAP/UDP, ADR 0014)
`POST coap://hub:5683/d/<device_id>/e` with a wire-format v1 frame and an HMAC tag keyed by the per-device secret. The piggybacked `2.04` reply carries authenticated ACKs, config and OTA items. Errors: `4.01` (unknown device or bad tag), `4.29` (rate limited), `4.00` and `4.04`.

## Errors
`ErrorResponse {code, message}` with status codes:

| Status | Codes |
|---|---|
| 400 | `invalid_request`, `invalid_json`, `invalid_config`, `invalid_release` |
| 401 | `unauthenticated` |
| 403 | `forbidden`, `denied` |
| 404 | `not_found` |
| 409 | `conflict` |
| 413 | `payload_too_large` |
