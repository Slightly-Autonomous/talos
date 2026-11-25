"""Run the Vision Node agent.

    python3 -m talos_vision --config /etc/talos/vision.json          # on the Pi (systemd unit)
    python3 -m talos_vision --config vision.dev.json --virtual DIR   # virtual node on the Mac

vision.json: {"deviceId": "cam_patio", "propertyId": "house-01", "hubUrl": "http://hub.local:8080",
              "tokenFile": "/etc/talos/device.token"}
The token file is the device's hub token, mode 0600, written by tools/provision/vision_node.py.
--virtual DIR: the lid is DIR/lid ("1" = open), the Pi reads a fixed 45 C, the clock counts as synced.
"""

from __future__ import annotations

import argparse
import json
import logging
import pathlib
import sys
import threading
import time

from . import board
from .agent import Agent, Config
from .hub import HubClient, urllib_transport
from .sources import (AlwaysSynced, FileLid, FixedPi, GpiodLid, MediaMtxStream, TimedatectlClock,
                      VcgencmdPi)

STEP_S = 0.02  # 20 ms: well inside the 50 ms lid debounce
MIN_SANE_UNIX_S = 1_767_225_600  # 2026-01-01: anything earlier is an unsynced clock


def main(argv: list | None = None) -> int:
    ap = argparse.ArgumentParser(description="TL-VN-001 Vision Node agent")
    ap.add_argument("--config", required=True)
    ap.add_argument("--virtual", metavar="DIR", help="fake lid, Pi and clock (development)")
    ap.add_argument("--mediamtx-api", default="http://127.0.0.1:9997")
    a = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s talos-vision %(message)s")
    log = logging.getLogger()

    raw = json.loads(pathlib.Path(a.config).read_text())
    token = pathlib.Path(raw["tokenFile"]).read_text().strip()
    cfg = Config(device_id=raw["deviceId"], property_id=raw["propertyId"])
    if a.virtual:
        state = pathlib.Path(a.virtual)
        state.mkdir(parents=True, exist_ok=True)
        lid, pi, clock = FileLid(state / "lid"), FixedPi(), AlwaysSynced()
    else:
        lid = GpiodLid(board.TAMPER_GPIO, board.TAMPER_OPEN_LEVEL)
        pi, clock = VcgencmdPi(), TimedatectlClock()

    # Boot epochs are Unix seconds: wait for a real clock (ADR 0020).
    while not (clock.synchronised() and time.time() > MIN_SANE_UNIX_S):
        log.info("waiting for NTP time sync before reporting")
        time.sleep(5)

    hub = HubClient(urllib_transport(raw["hubUrl"], token))
    agent = Agent(cfg, hub, MediaMtxStream(board.STREAM_PATH, a.mediamtx_api), pi, lid,
                  lambda: int(time.time() * 1000))
    log.info("reporting as %s, epoch %d", cfg.device_id, agent.epoch)

    def network() -> None:
        while True:
            agent.network_step(int(time.monotonic() * 1000))
            while hub.rejected:
                env, status, body = hub.rejected.pop(0)
                log.warning("hub refused %s (%s): %s", env.get("eventType"), status, body)
            time.sleep(0.1 if hub.reachable else 2.0)  # back off while the hub is away

    threading.Thread(target=network, name="hub", daemon=True).start()
    while True:
        agent.step(int(time.monotonic() * 1000))
        time.sleep(STEP_S)


if __name__ == "__main__":
    sys.exit(main())
