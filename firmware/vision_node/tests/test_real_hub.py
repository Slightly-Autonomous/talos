"""The agent against the real hub binary (dev config): set TALOS_HUB=bazel-bin/server/talos_hub.

Fake camera, fake lid, real HTTP: the hub must see the Vision Node healthy while it streams, degraded
when the stream stalls, and its lid opening must raise the enclosure tamper alarm.
"""
import os
import pathlib
import sys
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "hil"))

from talos_vision.agent import Agent, Config  # noqa: E402
from talos_vision.hub import HubClient, urllib_transport  # noqa: E402
from talos_vision.sources import FixedPi, StreamSample  # noqa: E402

HUB = os.environ.get("TALOS_HUB")


class Camera:
    def __init__(self):
        self.bytes, self.moving = 0, True

    def sample(self):
        if self.moving:
            self.bytes += 50_000
        return StreamSample(True, self.bytes)


class Lid:
    open = False

    def raw_open(self):
        return self.open


@unittest.skipUnless(HUB and pathlib.Path(HUB).exists(), "set TALOS_HUB to the talos_hub binary")
class AgentAgainstRealHub(unittest.TestCase):
    def setUp(self):
        from talos_hil.hub import DevHub

        self.hub = DevHub(HUB)
        self.addCleanup(self.hub.stop)
        url = f"http://127.0.0.1:{self.hub.api_port}"
        self.cam, self.lid = Camera(), Lid()
        cfg = Config("cam_patio", "house-01", stream_poll_ms=100, stall_ms=500)
        self.agent = Agent(cfg, HubClient(urllib_transport(url, "dev-cam-patio-token")), self.cam,
                           FixedPi(), self.lid, lambda: int(time.time() * 1000))

    def run_for(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            now = int(time.monotonic() * 1000)
            self.agent.step(now)
            self.agent.network_step(now)
            time.sleep(0.02)

    def health(self):
        for d in self.hub.get("/v1/health/devices")["devices"]:
            if d["deviceId"] == "cam_patio":
                return d
        self.fail("no cam_patio health")

    def test_streaming_lid_and_stall(self):
        self.run_for(1.0)
        self.assertEqual(self.health()["status"], "HEALTH_STATUS_HEALTHY", self.health())
        self.assertEqual(self.agent.hub.rejected, [])

        self.cam.moving = False  # the camera stops producing video
        self.run_for(1.5)
        h = self.health()
        self.assertEqual(h["status"], "HEALTH_STATUS_DEGRADED")
        self.assertIn("camera stream down", h["evidence"])

        self.lid.open = True
        self.run_for(0.5)
        self.assertEqual(self.health()["status"], "HEALTH_STATUS_FAULTED")
        # The enclosure tamper rule speaks on the patio speaker (deploy/config/hub.dev.yaml).
        state = self.hub.get("/v1/state")
        ops = [c.get("command", {}).get("operation") for c in state.get("commands", [])]
        self.assertIn("audio.speak", ops)
        self.assertEqual(self.agent.hub.rejected, [])


if __name__ == "__main__":
    unittest.main()
