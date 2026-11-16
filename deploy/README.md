# Deploying the Talos hub

The hub is the authority (ADR 0002/0010). Every deployment below runs the **same** `talos_hub` binary and serves the **same** HTTP API v1. Supabase and Railway are config switches, not code paths.

| Setting (`talos.server.v1.HubConfig`) | Values | Status |
|---|---|---|
| `storage.driver` | `STORAGE_DRIVER_SQLITE` | implemented (default) |
|  | `STORAGE_DRIVER_POSTGRES` | reserved: rejected with a clear error |
| `sync.target` | `SYNC_TARGET_NONE`, `SYNC_TARGET_SUPABASE` | implemented (run `connectors/supabase_sync`) |
| `auth` | `AUTH_MODE_LOCAL` | implemented |
|  | `AUTH_MODE_SUPABASE` | reserved |
| `role` | `HUB_ROLE_HUB` | implemented |
|  | `HUB_ROLE_RELAY` | reserved |

Environment overrides: `PORT`, `TALOS_LISTEN_HOST`, `TALOS_SQLITE_PATH`, `TALOS_CONFIG` (container entrypoint).

## 1. Local, no containers

```bash
bazel run //server:talos_hub -- --config $PWD/deploy/config/hub.dev.yaml
```

```bash
curl localhost:8080/v1/health
```

The dev config (`deploy/config/hub.dev.yaml`) uses **well-known development tokens**: `dev-owner-token` (owner), `dev-sync-token` (sync worker), `dev-node-07-token`, `dev-node-12-token`, `dev-patio-speaker-token`, `dev-lock-3-token`. It sets `dev_mode: true`, so the hub refuses to listen on anything but loopback.

## 2. Docker Compose

```bash
docker compose -f deploy/docker-compose.yml up --build
```

The hub listens on `127.0.0.1:8080`, and the journal lives in the `talos-data` volume. To add the Supabase copy:

1. Create a Supabase project and run `deploy/supabase/migrations/0001_talos_init.sql` in its SQL editor (or with `supabase db push`).
2. Copy `deploy/.env.example` to `deploy/.env` and fill in `SUPABASE_URL` and `SUPABASE_SERVICE_ROLE_KEY`.
3. Set `sync.target: SYNC_TARGET_SUPABASE` in your hub config, then run:

```bash
docker compose -f deploy/docker-compose.yml --profile supabase-sync up --build
```

The worker pulls `/v1/journal` and `/v1/state` with a read-only token and upserts them into Supabase tables protected by row-level security (members read their own properties). Supabase never sends commands; remote control goes through the hub API.

## 3. Railway

Create two services from this repository and set each service's config-as-code path:

- **hub**: `deploy/railway/hub/railway.json`. Attach a volume at `/data`. Set `TALOS_CONFIG` to a production config: real token hashes and `dev_mode` absent. The dev config refuses to start on Railway by design.
- **supabase-sync** (optional): `deploy/railway/supabase-sync/railway.json`, with `HUB_URL` (the hub's private Railway URL), `HUB_TOKEN`, `PROPERTY_ID`, `SUPABASE_URL` and `SUPABASE_SERVICE_ROLE_KEY`.

Running the hub in the cloud makes the cloud the authority for that property ("cloud-hosted hub"). That suits demos and simulation. A real home should run the hub locally and use Railway/Supabase only for the copy and, later, the remote relay (`HUB_ROLE_RELAY`).

## Device network and firmware releases (ADR 0014)

The dev config enables CoAP/UDP device ingress on port 5683, with insecure dev keys in `deploy/config/dev-device-keys.yaml`. In production, provision a random key per device and keep the keys file out of git (mode 0600).

Sign and publish a firmware release with your own key:

```bash
bazel run //server:talos_hub -- keygen
```

```bash
bazel run //server:talos_hub -- sign-release seed.hex owner-2026 universal_controller_v1 proto-a 2 $PWD/firmware.bin $PWD/release.json
```

Put the printed public key in `firmware_signing_keys`. Then POST `release.json` to `/v1/firmware/releases` and start a campaign with `POST /v1/firmware/campaigns {"deviceId":"node-12","version":2}`.

## Observability profile

```bash
docker compose -f deploy/docker-compose.yml --profile observability up --build
```

Prometheus (`127.0.0.1:9090`) scrapes `/metrics` with the dev metrics token. Grafana (`127.0.0.1:3000`, anonymous viewer) shows the provisioned "Talos hub" dashboard. Both are read-only engineering tools, never in the alarm path.

## Making tokens

```bash
bazel run //server:talos_hub -- hash-token "$(openssl rand -hex 32)"
```

Put only the printed hash in the config, and give the raw token to the person or device.
