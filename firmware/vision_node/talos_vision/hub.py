"""The hub's HTTP device API (docs/api.md), as one device: observations, ACKs, pending commands.

Observations go out in order. When the hub is unreachable they wait in a bounded queue and are resent
in the same order, with the same event ids, so the hub deduplicates any that did arrive. The oldest
are dropped first if the queue fills; the hub then sees a gap, never a reordering.

`send` only queues, so sensing never waits on the network; `flush` (the network thread) delivers.
"""

from __future__ import annotations

import collections
import json
import threading
import urllib.error
import urllib.request
from typing import Callable, Deque, Optional

Transport = Callable[[str, str, Optional[dict]], "tuple[int, Optional[dict]]"]


def urllib_transport(base: str, token: str, timeout_s: float = 5.0) -> Transport:
    def call(method: str, path: str, body: Optional[dict]) -> "tuple[int, Optional[dict]]":
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(base.rstrip("/") + path, data=data, method=method,
                                     headers={"Authorization": f"Bearer {token}",
                                              "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout_s) as r:
                raw = r.read()
                return r.status, json.loads(raw) if raw else None
        except urllib.error.HTTPError as e:
            try:
                return e.code, json.loads(e.read() or b"null")
            except ValueError:
                return e.code, None
        except (OSError, ValueError):
            return 0, None  # unreachable

    return call


class HubClient:
    MAX_QUEUE = 512

    def __init__(self, transport: Transport):
        self.t = transport
        self.queue: Deque[dict] = collections.deque(maxlen=self.MAX_QUEUE)
        self.rejected: list = []  # (envelope, status, body): bugs or misconfiguration, logged
        self.reachable = True
        self._lock = threading.Lock()

    def send(self, envelope: dict) -> None:
        with self._lock:
            self.queue.append(envelope)

    def flush(self) -> int:
        """Sends queued observations in order; stops at the first one the hub didn't get."""
        sent = 0
        while True:
            with self._lock:
                if not self.queue:
                    break
                env = self.queue[0]
            status, body = self.t("POST", "/v1/devices/observations", env)
            self.reachable = status != 0
            if status == 0 or status >= 500 or status == 429:
                break  # unreachable, failing or rate limited: keep it, try again later
            with self._lock:
                if self.queue and self.queue[0] is env:
                    self.queue.popleft()
            if status != 200:
                self.rejected.append((env, status, body))  # 4xx: resending would not help
            sent += 1
        return sent

    def commands(self) -> list:
        status, body = self.t("GET", "/v1/devices/commands", None)
        return list((body or {}).get("commands", [])) if status == 200 else []

    def ack(self, command_id: str, accepted: bool, detail: str) -> bool:
        status, _ = self.t("POST", "/v1/devices/acks",
                           {"commandId": command_id, "accepted": accepted, "detail": detail[:256]})
        return status == 200
