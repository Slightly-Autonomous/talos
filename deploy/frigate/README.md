# Frigate for Talos cameras (ADR 0020)

Frigate records the Vision Node's RTSP stream and detects people, cars and animals. Talos only
journals metadata and media references; video stays in Frigate. The hub serves snapshots and clips
to people through `GET /v1/media/...` (`docs/api.md`).

For now Frigate runs on the development Mac in Docker, with the CPU detector. The production
recorder hardware is decided later.

## 1. Generate the Frigate config from the camera map

`cameras.dev.json` maps a Frigate camera name to its Talos device, the device token, and the RTSP
URL. The password stays a `{FRIGATE_RTSP_PASSWORD}` placeholder, which Frigate fills in from its
environment.

```bash
mkdir -p deploy/frigate/config && pnpm --filter @talos/frigate-connector exec tsx src/gen-config.ts ../../deploy/frigate/cameras.dev.json > deploy/frigate/config/config.yml
```

## 2. Start Frigate

```bash
FRIGATE_RTSP_PASSWORD=dev-rtsp-password docker compose -f deploy/docker-compose.yml --profile vision up frigate
```

- **Web UI:** https://localhost:8971. The first start prints the admin password in the log.
- **API:** http://127.0.0.1:5000. It is unauthenticated, so it is bound to localhost only.

## 3. Run the connector

The connector posts detections and heartbeats for `cam_patio_video`, executes bookmarks and
reports their clips:

```bash
HUB_URL=http://127.0.0.1:8080 FRIGATE_URL=http://127.0.0.1:5000 PROPERTY_ID=house-01 CAMERA_MAP="$(cat deploy/frigate/cameras.dev.json)" pnpm --filter @talos/frigate-connector start
```

## Without a camera

The virtual Vision Node (`firmware/vision_node/README.md`) publishes a test pattern on
`rtsp://…:8554/cam` from this Mac. `host.docker.internal` in the camera map is how Frigate in
Docker reaches it.

## Production

- Use a real `FRIGATE_RTSP_PASSWORD`, set the same on the camera, and keep both out of git.
- Keep port 5000 off the network.
- Point the hub at Frigate with `media.frigate_url`.
- Choose the detector with `gen-config.ts --hailo8l` once a Hailo-8L is fitted.
