# ADR 0015: Integrations, AI connector and remote access

**Status:** Accepted (2026-11-16). Implements most of Phase 7 (spec §24, §29).

## Decision
- **Supabase sign-in (`AUTH_MODE_SUPABASE`)** is now implemented and additive: local tokens keep working.
  - The hub verifies Supabase-issued **HS256** JWTs: signature, `exp`, `nbf`, and `aud` (default `authenticated`). It rejects every other algorithm, including `none`.
  - It maps `sub` to a configured principal (`supabase_auth.users`). A signed-in user who is not mapped is rejected.
  - The JWT secret is read from the env var named in config.
  - A JWT only authenticates. The principal's grants in `core/policy` still decide every action.
- **MCP server** (`connectors/mcp_server`, `@modelcontextprotocol/sdk` 1.32.2, stdio).
  - **Read and plan tools:** `property_overview`, `search_activity`, `device_health`, `preview_rule` (dry-run; it says a human must save).
  - **Request tools:** `request_speaker`, `request_notification`.
  - **No unlock, disarm or rule-saving tools exist.** The connector also runs with an **AGENT** principal token, so even a modified connector is capped by hub policy: no security-critical actions, and outputs only up to `disruptive_output`.
  - Every tool result is framed as untrusted hub data. A prompt-injection string in device data comes back quoted, never as instructions (spec §24).
- **Webhook notifier** (`connectors/webhook_notifier`): an out-of-process device adapter for `notify`. It polls its own commands through the HTTP device API, posts them to an ntfy or JSON webhook over HTTPS, and ACKs 2xx as accepted and anything else as rejected. Notify stays ack-only and is never "verified".
- **Frigate connector** (`connectors/frigate`): polls Frigate's `/api/events` and posts `camera.object_detected` observations with each mapped camera's own device token. Event ids are deduplicated. Video stays in Frigate.
- **API reference:** `docs/api.md`.

## Deferred: relay role (`HUB_ROLE_RELAY` stays reserved)
A relay would let remote clients reach a home hub without opening ports, by having the hub hold an outbound connection to a cloud relay. It touches TLS termination, connection multiplexing, session revocation and a hosted service someone has to operate, so it deserves its own design and security review.

Remote access today, without weakening the authority model:
- **Read-only from anywhere:** the Supabase copy with row-level security (ADR 0010), plus Supabase sign-in.
- **Full control:** reach the hub's API over a VPN or tunnel you control (e.g. WireGuard or Tailscale), with TLS in front.

## Not done
- Supabase asymmetric JWT signing keys (ES256/RS256 via JWKS). Only the HS256 project secret is supported today.
- Native mobile app.
- An ONVIF adapter for cameras without Frigate.
- MCP over HTTP/OAuth transports.
- Verifying a specific vendor AI client's MCP compatibility. Talos only claims a standard MCP stdio server.
