"""The Vision Node agent (ADR 0020): the one producer for the camera's own device ("<cam>").

It reports, over the hub's HTTP device API:
- `tamper.enclosure`: the lid switch, debounced, the moment it changes, plus its state at start;
- `camera.stream`: whether MediaMTX's camera path is producing video, with resolution, frame rate,
  bitrate, SoC temperature and throttling, on every change and once a minute;
- `device.health`: a heartbeat once a minute (well inside the hub's freshness age).

Boot epochs are Unix seconds (ADR 0008/0014). The Zero has no RTC, so `__main__` starts the agent only
after NTP has synchronised; a later boot then always has a larger epoch. Frigate's view of the same
camera ("<cam>_video") is a separate device reported by the Frigate connector.

The agent is driven with a monotonic clock, so tests run it in simulated time: `step(now_ms)` senses
(every 20 ms, never touching the network) and `network_step(now_ms)` delivers and polls commands on
its own thread, so an unreachable hub can never delay the lid.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Optional

from . import board
from .hub import HubClient
from .sources import Lid, Pi, Stream


@dataclass
class Config:
    device_id: str
    property_id: str
    heartbeat_ms: int = 60_000
    stream_report_ms: int = 60_000
    stream_poll_ms: int = 1_000
    # No new bytes for this long while "ready" = a stalled camera, reported as down.
    stall_ms: int = 5_000
    command_poll_ms: int = 5_000
    debounce_ms: int = board.TAMPER_DEBOUNCE_MS


class Agent:
    def __init__(self, cfg: Config, hub: HubClient, stream: Stream, pi: Pi, lid: Lid,
                 wall_ms: Callable[[], int]):
        self.cfg, self.hub, self.stream, self.pi, self.lid, self.wall_ms = cfg, hub, stream, pi, lid, wall_ms
        self.epoch = wall_ms() // 1000
        self.seq = 0
        # Lid debounce.
        self._lid_candidate: Optional[bool] = None
        self._lid_since = 0
        self.lid_reported: Optional[bool] = None
        # Stream.
        self._last_bytes: Optional[int] = None
        self._last_bytes_at = 0
        self._last_progress_at: Optional[int] = None
        self._bitrate_kbps = 0
        self.stream_up_reported: Optional[bool] = None
        self._next_stream_poll = 0
        self._next_stream_report = 0
        self._next_heartbeat = 0
        self._next_commands = 0
        self.unsupported_commands = 0

    # ---- envelopes ------------------------------------------------------------------------------------

    def _envelope(self, capability: str, event_type: str, payload: dict) -> dict:
        self.seq += 1
        return {
            "schemaVersion": 1,
            "propertyId": self.cfg.property_id,
            "source": {"deviceId": self.cfg.device_id, "capabilityId": capability},
            "eventType": event_type,
            "eventId": f"{self.cfg.device_id}:{self.epoch}:{self.seq}",
            "position": {"bootEpoch": self.epoch, "sequence": str(self.seq)},
            "observedAtUnixMs": str(self.wall_ms()),
            "payload": payload,
        }

    # ---- duties ---------------------------------------------------------------------------------------

    def _lid(self, now: int) -> None:
        raw = self.lid.raw_open()
        if raw != self._lid_candidate:
            self._lid_candidate, self._lid_since = raw, now
            return
        if now - self._lid_since < self.cfg.debounce_ms or raw == self.lid_reported:
            return
        self.lid_reported = raw
        self.hub.send(self._envelope("tamper.enclosure", "tamper.opened" if raw else "tamper.closed",
                                     {"tamper": {"open": raw}}))

    def _stream_up(self, now: int) -> bool:
        s = self.stream.sample()
        if s is None or not s.ready:
            self._last_bytes, self._last_progress_at, self._bitrate_kbps = None, None, 0
            return False
        if self._last_bytes is not None and s.bytes_produced > self._last_bytes:
            dt = max(1, now - self._last_bytes_at)
            self._bitrate_kbps = (s.bytes_produced - self._last_bytes) * 8 // dt  # bits/ms = kbit/s
            self._last_progress_at = now
        elif self._last_progress_at is None:
            self._last_progress_at = now  # first sample of a ready path: give it stall_ms to move
        self._last_bytes, self._last_bytes_at = s.bytes_produced, now
        return now - self._last_progress_at < self.cfg.stall_ms

    def _stream(self, now: int) -> None:
        if now < self._next_stream_poll:
            return
        self._next_stream_poll = now + self.cfg.stream_poll_ms
        up = self._stream_up(now)
        if up == self.stream_up_reported and now < self._next_stream_report:
            return
        self.stream_up_reported = up
        self._next_stream_report = now + self.cfg.stream_report_ms
        status = {"up": up, "socMilliC": self.pi.soc_milli_c(), "throttled": self.pi.throttled()}
        if up:
            status.update(width=board.STREAM_WIDTH, height=board.STREAM_HEIGHT,
                          fpsMilli=board.STREAM_FPS * 1000, bitrateKbps=self._bitrate_kbps)
        self.hub.send(self._envelope("camera.stream", "camera.stream_status", {"streamStatus": status}))

    def _heartbeat(self, now: int) -> None:
        if now < self._next_heartbeat:
            return
        self._next_heartbeat = now + self.cfg.heartbeat_ms
        self.hub.send(self._envelope("device.health", "device.heartbeat", {"heartbeat": {"batteryMv": 0}}))

    def _commands(self, now: int) -> None:
        # Nothing in the operation catalog targets this device yet (maintenance is hub-side):
        # anything that arrives is refused, so it never sits as "dispatched".
        if now < self._next_commands:
            return
        self._next_commands = now + self.cfg.command_poll_ms
        for c in self.hub.commands():
            self.unsupported_commands += 1
            self.hub.ack(str(c.get("id", "")), False, f"unsupported on TL-VN-001: {c.get('operation')}")

    def step(self, now_ms: int) -> None:
        """Sensing and reporting into the queue; no network I/O."""
        self._lid(now_ms)
        self._heartbeat(now_ms)
        self._stream(now_ms)

    def network_step(self, now_ms: int) -> None:
        """Delivery and commands (the network thread)."""
        self.hub.flush()
        if self.hub.reachable:
            self._commands(now_ms)
