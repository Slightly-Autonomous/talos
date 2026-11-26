#!/usr/bin/env bash
# Virtual Vision Node on the Mac (ADR 0020): MediaMTX, an ffmpeg test pattern published as "cam",
# and the agent with a fake lid.
#   firmware/vision_node/dev/run_virtual.sh
# MediaMTX: on PATH, or MEDIAMTX=/path/to/mediamtx. ffmpeg: a working local one, or else the Frigate
# image's ffmpeg in Docker (FFMPEG_IMAGE). Open the lid: echo 1 > /tmp/talos-vision/lid
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
mediamtx="${MEDIAMTX:-$(command -v mediamtx || echo "$HOME/.cache/talos-tools/mediamtx/mediamtx")}"
[ -x "$mediamtx" ] || { echo "mediamtx not found (set MEDIAMTX)" >&2; exit 2; }
state="${TALOS_VISION_STATE:-/tmp/talos-vision}"
mkdir -p "$state"
"$mediamtx" "$here/dev/mediamtx.dev.yml" &
pids=($!)
trap 'kill "${pids[@]}" 2>/dev/null || true; docker rm -f talos-vn-pattern >/dev/null 2>&1 || true' EXIT
sleep 1
pattern=(-hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=15"
         -c:v libx264 -preset ultrafast -tune zerolatency -g 30 -b:v 2M -f rtsp)
if command -v ffmpeg >/dev/null && ffmpeg -hide_banner -version >/dev/null 2>&1; then
  ffmpeg "${pattern[@]}" rtsp://127.0.0.1:8554/cam &
  pids+=($!)
else
  image="${FFMPEG_IMAGE:-ghcr.io/blakeblackshear/frigate:0.17.2}"
  echo "local ffmpeg unusable: publishing from $image"
  docker run -d --rm --name talos-vn-pattern --add-host host.docker.internal:host-gateway \
    --entrypoint "${FFMPEG_IN_IMAGE:-/usr/lib/ffmpeg/7.0/bin/ffmpeg}" "$image" \
    "${pattern[@]}" rtsp://publisher:dev-publish@host.docker.internal:8554/cam >/dev/null
fi
cd "$here"
python3 -m talos_vision --config dev/vision.dev.json --virtual "$state"
