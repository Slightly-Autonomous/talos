import unittest

from talos_vision.agent import Agent, Config
from talos_vision.hub import HubClient
from talos_vision.sources import FixedPi, StreamSample, parse_temp, parse_throttled

T0_WALL = 1_800_000_000_000


class FakeHub:
    """The hub's device API: records observations, can be unreachable, can queue commands."""

    def __init__(self):
        self.up = True
        self.observations = []
        self.acks = []
        self.commands = []

    def __call__(self, method, path, body):
        if not self.up:
            return 0, None
        if path == "/v1/devices/observations":
            if body["eventId"] in {o["eventId"] for o in self.observations}:
                return 200, {"status": "duplicate"}
            self.observations.append(body)
            return 200, {"status": "accepted"}
        if path == "/v1/devices/commands":
            out, self.commands = self.commands, []
            return 200, {"commands": out}
        if path == "/v1/devices/acks":
            self.acks.append(body)
            return 200, {"status": "accepted"}
        return 404, None

    def of(self, capability):
        return [o for o in self.observations if o["source"]["capabilityId"] == capability]


class FakeStream:
    def __init__(self):
        self.ready = True
        self.bytes = 0
        self.rate = 500  # bytes per ms (4 Mbit/s)
        self.mediamtx_up = True

    def advance(self, ms):
        if self.ready:
            self.bytes += self.rate * ms

    def sample(self):
        return StreamSample(self.ready, self.bytes) if self.mediamtx_up else None


class FakeLid:
    def __init__(self):
        self.open = False

    def raw_open(self):
        return self.open


class Rig:
    def __init__(self):
        self.hub = FakeHub()
        self.stream = FakeStream()
        self.lid = FakeLid()
        self.now = 0
        self.agent = Agent(Config("cam_patio", "house-01"), HubClient(self.hub), self.stream,
                           FixedPi(51_250, 0), self.lid, lambda: T0_WALL + self.now)

    def run(self, ms, step=20):
        end = self.now + ms
        while self.now < end:
            self.stream.advance(step)
            self.now += step
            self.agent.step(self.now)
            self.agent.network_step(self.now)


class Reporting(unittest.TestCase):
    def test_start_reports_lid_heartbeat_and_stream_with_unix_second_epoch(self):
        r = Rig()
        r.run(3_000)
        self.assertEqual(r.agent.epoch, T0_WALL // 1000)
        lid = r.hub.of("tamper.enclosure")
        self.assertEqual(len(lid), 1)
        self.assertEqual(lid[0]["eventType"], "tamper.closed")
        self.assertEqual(len(r.hub.of("device.health")), 1)
        stream = r.hub.of("camera.stream")
        self.assertEqual(stream[0]["eventType"], "camera.stream_status")
        self.assertTrue(stream[-1]["payload"]["streamStatus"]["up"])
        s = stream[-1]["payload"]["streamStatus"]
        self.assertEqual((s["width"], s["height"], s["fpsMilli"], s["socMilliC"]),
                         (1920, 1080, 15_000, 51_250))
        # Ids are namespaced by the device and positions strictly increase.
        seqs = [int(o["position"]["sequence"]) for o in r.hub.observations]
        self.assertEqual(seqs, sorted(seqs))
        self.assertTrue(all(o["eventId"].startswith("cam_patio:") for o in r.hub.observations))
        self.assertEqual(len(set(seqs)), len(seqs))

    def test_periodic_reports_stay_inside_the_hubs_freshness_age(self):
        r = Rig()
        r.run(10 * 60_000, step=100)
        self.assertGreaterEqual(len(r.hub.of("device.health")), 10)
        self.assertGreaterEqual(len(r.hub.of("camera.stream")), 10)

    def test_lid_is_debounced_and_reported_at_once(self):
        r = Rig()
        r.run(1_000)
        r.lid.open = True
        r.run(20)  # a 20 ms bounce
        r.lid.open = False
        r.run(500)
        self.assertEqual([o["eventType"] for o in r.hub.of("tamper.enclosure")], ["tamper.closed"])
        r.lid.open = True
        r.run(100)
        lid = r.hub.of("tamper.enclosure")
        self.assertEqual(lid[-1]["eventType"], "tamper.opened")
        self.assertEqual(lid[-1]["payload"], {"tamper": {"open": True}})

    def test_a_stalled_or_missing_stream_is_down(self):
        r = Rig()
        r.run(3_000)
        r.stream.rate = 0  # MediaMTX says ready, but no new bytes: a stuck camera
        r.run(8_000)
        self.assertFalse(r.hub.of("camera.stream")[-1]["payload"]["streamStatus"]["up"])
        r.stream.rate = 500
        r.run(3_000)
        self.assertTrue(r.hub.of("camera.stream")[-1]["payload"]["streamStatus"]["up"])
        r.stream.mediamtx_up = False  # MediaMTX itself gone
        r.run(2_000)
        last = r.hub.of("camera.stream")[-1]["payload"]["streamStatus"]
        self.assertFalse(last["up"])
        self.assertNotIn("width", last)

    def test_offline_reports_are_kept_and_resent_in_order(self):
        r = Rig()
        r.run(2_000)
        before = len(r.hub.observations)
        r.hub.up = False
        r.lid.open = True
        r.run(70_000, step=100)  # lid opened and a heartbeat while the hub is away
        self.assertEqual(len(r.hub.observations), before)
        r.hub.up = True
        r.run(200)
        self.assertTrue(r.agent.hub.reachable)
        sent = r.hub.observations[before:]
        self.assertIn("tamper.opened", [o["eventType"] for o in sent])
        seqs = [int(o["position"]["sequence"]) for o in r.hub.observations]
        self.assertEqual(seqs, sorted(seqs))

    def test_sensing_never_touches_the_network(self):
        r = Rig()
        r.hub.up = False
        calls = []
        r.agent.hub.t = lambda *a: calls.append(a) or (0, None)
        r.lid.open = True
        for t in range(20, 2_000, 20):
            r.agent.step(t)
        self.assertEqual(calls, [])
        self.assertTrue(any(e["eventType"] == "tamper.opened" for e in r.agent.hub.queue))

    def test_commands_are_refused_never_left_dispatched(self):
        r = Rig()
        r.hub.commands = [{"id": "c1", "operation": "camera.snapshot"}]
        r.run(6_000)
        self.assertEqual(r.hub.acks, [{"commandId": "c1", "accepted": False,
                                       "detail": "unsupported on TL-VN-001: camera.snapshot"}])


class HubQueue(unittest.TestCase):
    def test_a_refused_report_is_dropped_and_logged_not_retried_forever(self):
        calls = []

        def t(method, path, body):
            calls.append(body["eventId"])
            return (400, {"code": "invalid_request"}) if body["eventId"] == "bad" else (200, {})

        h = HubClient(t)
        h.send({"eventId": "bad"})
        h.send({"eventId": "good"})
        self.assertEqual(h.flush(), 2)
        self.assertEqual(calls, ["bad", "good"])
        self.assertEqual(len(h.rejected), 1)

    def test_queue_is_bounded(self):
        h = HubClient(lambda m, p, b: (0, None))
        for i in range(HubClient.MAX_QUEUE + 10):
            h.send({"eventId": str(i)})
        self.assertEqual(len(h.queue), HubClient.MAX_QUEUE)
        self.assertEqual(h.queue[0]["eventId"], "10")


class Parsing(unittest.TestCase):
    def test_vcgencmd_output(self):
        self.assertEqual(parse_temp("temp=48.3'C\n"), 48_300)
        self.assertIsNone(parse_temp("error"))
        self.assertEqual(parse_throttled("throttled=0x50005\n"), 0x50005)
        self.assertIsNone(parse_throttled(""))


if __name__ == "__main__":
    unittest.main()
