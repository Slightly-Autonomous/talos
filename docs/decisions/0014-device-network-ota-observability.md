# ADR 0014: Device network, signed OTA delivery and observability

**Status:** Accepted (2026-11-16). Implements most of Phase 6 (spec §19, §22, §27, §36, §45).

## Decision
### Device transport
- **Wire format v1** (`firmware/portable/wire.h`), shared by firmware and hub:
  - Fixed little-endian event records (28 B) and typed downlink items: ACK, config, OTA offer, OTA chunk, reboot.
  - Explicit lengths and range checks.
  - An **8-byte truncated HMAC-SHA256 tag**, keyed by a per-device secret provisioned at claim. Thread adds link-layer security underneath.
  - **Each downlink tag also covers the request's tag**, binding every reply to the request it answers so old downlinks can't be replayed.
- **CoAP (RFC 7252 subset)** in `common/coap`: header, token, delta-encoded options, Uri-Path and payload. Blockwise, observe and DTLS are out of scope.
- **Hub ingress** (`server/coap_server`):
  - `POST /d/<device>/e` → device key lookup → HMAC-verified frame → per-device token-bucket rate limit (4.29) → `HubService::device_exchange` → piggybacked `2.04` reply with authenticated downlink. The Uri-Path device id is unauthenticated, so only verified frames spend a device's bucket. Unknown devices and bad tags spend a per-peer bucket and then get 4.29.
  - An HMAC proves origin, not freshness. Only a **fresh** frame (a tag not among the device's last 256, and at least one `(boot_epoch, seq)` the hub has not seen) spends the device's bucket. A replay or retransmission spends a per-peer replay bucket, is still deduplicated and ACKed, and gets no config or OTA downlink (so a replay cannot stamp `last_offer`). A LAN attacker replaying a sniffed frame mutes only its own address.
  - Telemetry (last seen, battery, OTA and config state) changes only for fresh evidence: journaled kinds only when ingest accepts them, and other kinds only once per `(boot_epoch, seq)` within a bounded window. Replayed frames refresh nothing. An uplink counter remains future work.
  - Campaigns are refused when the release's `board_id` or `board_revision` does not match the device's declared board, because the OTA offer itself carries no board identity.
  - Releases are keyed by `(board_id, board_revision, version)` and never replaced (409); versions are parsed strictly (1..4294967295, no wrap). A campaign pins the release's board, revision and SHA-256 at start, and every exchange re-checks them against the stored release and the device's declared board; a mismatch fails the campaign instead of offering another image.
  - Unknown devices and bad tags get `4.01`, and nothing is ingested.
  - A retransmitted CON gets the cached reply instead of being processed twice. Only replies to authenticated requests are cached, keyed by peer and message id and matched on the identical request bytes, so a datagram spoofed from the node's address cannot poison the reply the node gets. Events are also deduplicated by `(boot_epoch, seq)` in the engine.
  - **Firmware contract** (positions, ADR 0008 amendment): `boot_epoch` is a persisted counter incremented by exactly 1 per boot, never time-based; `seq` restarts each boot and increments by 1 per event within 32 bits. The hub refuses an epoch more than 64 past the newest it has seen, or a sequence more than 2^20 past the highest in its boot, and shows the device `FAULTED` when its reports are refused or land behind its record. After a reflash that resets the counter, the owner resets the device's sequence state and rotates its key. An authenticated uplink counter (checked before any work) remains future work.
  - Every authenticated event is ACKed, even ones ingest rejected, so a bad frame is never retried forever.
- **Firmware link** (`fw::CoapLink`): each event is a CON POST over an `IDatagram` HAL. The runtime's queue owns retries; a lost exchange is simply resent.
- **Sleepy-node model:** the device always initiates. The hub piggybacks ACKs, pending config and OTA data on the device's own messages. During OTA, the firmware reports progress once per batch of chunks, so the hub streams the next batch.

### Device configuration
`POST /v1/devices/{id}/config` sets the desired runtime config (heartbeat, environment interval, impact threshold).
- It's owner-only, because it can weaken detection.
- Revision numbers are hub seconds, so they stay monotonic across hub restarts.
- The config is delivered until the device reports `kConfigApplied` with that revision.

### Signed OTA delivery
- Releases (`talos.v1.FirmwareRelease` + image) are imported with `POST /v1/firmware/releases`, which is owner-only.
- The hub checks the image size and SHA-256 against the manifest, then verifies an **Ed25519** signature (libsodium) over a canonical message (`talos-firmware-release-v1`, board, revision, version, size, digest) against `firmware_signing_keys` in the hub config.
- **Owners hold their own keys** (`talos_hub keygen`, `talos_hub sign-release`). No vendor key is baked in.
- `POST /v1/firmware/campaigns` targets a device. The hub offers the release on the device's next contact, streams chunks from the device's reported offset, and tracks the campaign state: offered → downloading → pending_verify → done, or failed / rolled_back.
- The device verifies the digest and runs the A/B trial-boot and rollback from ADR 0013.

### Observability plane
- `DeviceRegistry` keeps bounded, in-memory telemetry per device: last seen, battery, power mode, slot modules and faults, OTA state, running version, applied config, and the last 64 diagnostics. **It never writes to the journal.**
- `project_health` maps telemetry to healthy, degraded, offline, low_power, faulted or maintenance, with evidence and a recommended action. Devices that only use the HTTP device API fall back to the freshness of their `device.health` capability. A device that has never reported is **offline**, never "healthy".
- API: `GET /v1/health/devices` and `GET /v1/diagnostics?device_id=`. The web Live view shows the projection, and Engineering shows diagnostics.
- Metrics: `GET /metrics` (bearer-authenticated) in Prometheus text format with **bounded labels only**: outcome names, never device or event ids. The `observability` Compose profile adds Prometheus and a provisioned, read-only Grafana dashboard. Grafana is engineering tooling, never in the alarm path.

## Not done (honest limits)
- **Real Thread:** the ESP32-H2 build runs the CoAP link over OpenThread UDP (`firmware/esp_idf/main/thread_link.cc`, 2026-11-22). It runs as an MTD, a sleepy end device or rx-on depending on the power profile, with credentials from diag `thread`. It is compile-checked in CI only. `tools/otns/thread_latency.py` measures attach, deep-sleep wake-to-reply, polling round trips and parent loss on the real stack in OTNS. Run on 2026-11-22 (`hardware/results/otns.md`): wake to first reply 0.23–0.33 s, sleepy-child round trip 0.16 s or less, re-attach after parent loss 1.5–4 s, all in simulation. The CoAP path itself is exercised over loopback UDP with real firmware on the virtual board.
- **On-device signature verification:** the node verifies the digest delivered over the authenticated channel. Verifying the Ed25519 manifest on the node itself, and secure boot/eFuse policy, need silicon and a key-management decision (spec §55 P0).
- **Persistence:** desired device config and campaigns live in memory and are lost on hub restart. Devices keep their applied config, and campaigns must be restarted.
- **Provisioning:** device keys come from a YAML file. The claim/commissioning flow that generates and stores them (spec §37) is still to do.
- **Telemetry export:** no OpenTelemetry traces exporter yet. Correlation ids are present on diagnostics and the journal.
- **Fuzzing:** randomized decoder tests run in gtest. libFuzzer and long-duration soak runs are not set up.
