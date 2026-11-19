"""serial_bridge: frames to and from the hub, console sharing, and the real path end to end."""

import base64
import os
import socket
import time
import unittest

from talos_hil.console import Console, ProcessTransport, TcpTransport, Transport
from talos_hil.serial_bridge import Bridge


class MemoryNode(Transport):
    """A node whose output we script and whose input we record."""

    def __init__(self):
        super().__init__()
        self.written: list[str] = []

    def emit(self, line: str) -> None:
        self._feed(line)

    def _write(self, data: str) -> None:
        self.written.append(data.rstrip("\n"))

    def close(self) -> None:
        self._feed(None)


def wait_for(pred, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return True
        time.sleep(0.02)
    return pred()


class BridgeUnit(unittest.TestCase):
    def setUp(self):
        self.hub = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.hub.bind(("127.0.0.1", 0))
        self.hub.settimeout(2)
        self.node = MemoryNode()
        self.bridge = Bridge(self.node, self.hub.getsockname(), console_port=0).start()

    def tearDown(self):
        self.bridge.stop()
        self.hub.close()

    def test_frames_go_to_the_hub_and_replies_come_back_as_frames(self):
        self.node.emit("@F " + base64.b64encode(b"\x44\x02hello").decode())
        data, addr = self.hub.recvfrom(4096)
        self.assertEqual(data, b"\x44\x02hello")
        self.hub.sendto(b"\x64\x44reply", addr)
        self.assertTrue(wait_for(lambda: any(w.startswith("@F ") for w in self.node.written)))
        frame = next(w for w in self.node.written if w.startswith("@F "))
        self.assertEqual(base64.b64decode(frame[3:]), b"\x64\x44reply")

    def test_garbled_frames_are_counted_not_forwarded(self):
        self.node.emit("@F not*base64")
        self.assertTrue(wait_for(lambda: self.bridge.bad_frames == 1))
        self.assertEqual(self.bridge.frames_up, 0)

    def test_console_clients_see_replies_and_cannot_inject_frames(self):
        client = TcpTransport("127.0.0.1", self.bridge.console_port)
        try:
            self.assertTrue(wait_for(lambda: self.bridge.clients))
            client.write_line("id")
            client.write_line("@F AAAA")
            self.assertTrue(wait_for(lambda: "id" in self.node.written))
            time.sleep(0.1)
            self.assertNotIn("@F AAAA", self.node.written)
            self.node.emit('@T {"cmd":"id","done":true,"ok":true}')
            self.assertEqual(client.read_line(timeout=2), '@T {"cmd":"id","done":true,"ok":true}')
        finally:
            client.close()


BENCH_NODE = os.environ.get("TALOS_BENCH_NODE")
HUB = os.environ.get("TALOS_HUB")


@unittest.skipUnless(BENCH_NODE and HUB, "set TALOS_BENCH_NODE and TALOS_HUB (bazel build "
                     "//firmware/sim:bench_node //server:talos_hub) to run the end-to-end test")
class EndToEnd(unittest.TestCase):
    def test_virtual_node_reaches_the_real_hub_through_the_bridge(self):
        from talos_hil.hub import DEV_KEYS, DevHub

        hub = DevHub(HUB)
        node = ProcessTransport([BENCH_NODE, "--slot0=TL-MOD-004",
                                 f"--provision=node-12:{DEV_KEYS['node-12']}"])
        bridge = Bridge(node, ("127.0.0.1", hub.coap_port), console_port=0).start()
        console = Console(TcpTransport("127.0.0.1", bridge.console_port))
        try:
            reply = console.command("identity")
            self.assertEqual(reply.record("identity")["device_id"], "node-12")

            def seen():
                devices = (hub.get("/v1/health/devices") or {}).get("devices", [])
                node12 = next((d for d in devices if d.get("deviceId") == "node-12"), {})
                return node12.get("status") == "HEALTH_STATUS_HEALTHY" and node12.get("lastSeenUnixMs", "0") != "0"

            self.assertTrue(wait_for(seen, timeout=20), hub.get("/v1/health/devices"))
            self.assertGreater(bridge.frames_up, 0)
            self.assertGreater(bridge.frames_down, 0)  # ACKs came back
        finally:
            console.close()
            bridge.stop()
            node.close()
            hub.stop()


if __name__ == "__main__":
    unittest.main()
