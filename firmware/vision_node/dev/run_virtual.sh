#!/usr/bin/env bash
# Virtual Vision Node on the Mac: MediaMTX with a test pattern, plus the agent with a fake lid.
#   firmware/vision_node/dev/run_virtual.sh          (needs mediamtx and ffmpeg on PATH)
# Open the lid:  echo 1 > /tmp/talos-vision/lid      Close it:  echo 0 > /tmp/talos-vision/lid
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
for tool in mediamtx ffmpeg; do
  command -v "$tool" >/dev/null || { echo "$tool not found (brew install $tool)" >&2; exit 2; }
done
state="${TALOS_VISION_STATE:-/tmp/talos-vision}"
mkdir -p "$state"
mediamtx "$here/dev/mediamtx.dev.yml" &
mtx=$!
trap 'kill $mtx 2>/dev/null' EXIT
cd "$here"
python3 -m talos_vision --config dev/vision.dev.json --virtual "$state"
