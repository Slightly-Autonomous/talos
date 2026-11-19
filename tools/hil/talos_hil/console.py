"""Line transports to a Talos node and a client for its diag console.

The node's USB-serial-JTAG port carries three kinds of line (docs/decisions/0016):
    "@T {json}"      diag replies; a command's replies end with {"done": true, ...}
    "@F <base64>"    CoAP datagrams to and from the hub (serial_bridge.py relays them)
    anything else    ESP-IDF log lines
The same lines come from firmware/sim:bench_node, so every tool here runs against the
virtual board in CI and against a real board on the bench.
"""

from __future__ import annotations

import json
import queue
import socket
import subprocess
import threading
import time
from dataclasses import dataclass, field


class Transport:
    """A line-oriented, thread-safe connection. Reads come from a background thread."""

    def __init__(self):
        self._lines: queue.Queue[str | None] = queue.Queue()
        self._write_lock = threading.Lock()
        self.closed = False

    def _feed(self, line: str | None) -> None:
        self._lines.put(line)

    def read_line(self, timeout: float | None = None) -> str | None:
        """Next line without its newline, or None on timeout or close."""
        try:
            line = self._lines.get(timeout=timeout)
        except queue.Empty:
            return None
        if line is None:
            self.closed = True
        return line

    def write_line(self, line: str) -> None:
        with self._write_lock:
            self._write(line.rstrip("\n") + "\n")

    def _write(self, data: str) -> None:
        raise NotImplementedError

    def close(self) -> None:
        raise NotImplementedError


class ProcessTransport(Transport):
    """A subprocess speaking the protocol on stdin/stdout (firmware/sim:bench_node)."""

    def __init__(self, argv: list[str]):
        super().__init__()
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, bufsize=1)
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self) -> None:
        assert self.proc.stdout
        for line in self.proc.stdout:
            self._feed(line.rstrip("\n"))
        self._feed(None)

    def _write(self, data: str) -> None:
        assert self.proc.stdin
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.stdin.close()  # type: ignore[union-attr]
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


class SerialTransport(Transport):
    """The board's USB-serial-JTAG port (/dev/cu.usbmodem* on macOS). Needs pyserial."""

    def __init__(self, port: str, baud: int = 115200):
        super().__init__()
        import serial  # noqa: PLC0415 - only needed with real hardware

        self.ser = serial.Serial(port, baud, timeout=0.1)
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self) -> None:
        buf = b""
        while not self.closed:
            try:
                chunk = self.ser.read(4096)
            except Exception:  # port vanished (board reset, unplugged)
                break
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                self._feed(raw.rstrip(b"\r").decode("utf-8", errors="replace"))
        self._feed(None)

    def _write(self, data: str) -> None:
        self.ser.write(data.encode())

    def close(self) -> None:
        self.closed = True
        self.ser.close()


class TcpTransport(Transport):
    """The console port that serial_bridge.py exposes while it owns the serial port."""

    def __init__(self, host: str, port: int):
        super().__init__()
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(None)
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self) -> None:
        buf = b""
        while True:
            try:
                chunk = self.sock.recv(4096)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                self._feed(raw.decode("utf-8", errors="replace"))
        self._feed(None)

    def _write(self, data: str) -> None:
        self.sock.sendall(data.encode())

    def close(self) -> None:
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()


@dataclass
class Reply:
    cmd: str
    records: list[dict] = field(default_factory=list)
    done: dict = field(default_factory=dict)
    logs: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return bool(self.done.get("ok"))

    def record(self, t: str) -> dict | None:
        return next((r for r in self.records if r.get("t") == t), None)


class ConsoleError(Exception):
    pass


class Console:
    """Sends diag commands and collects their "@T" records up to the done record."""

    def __init__(self, transport: Transport):
        self.t = transport

    def command(self, cmd: str, timeout: float = 10.0) -> Reply:
        self.t.write_line(cmd)
        reply = Reply(cmd)
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise ConsoleError(f"no done record for {cmd!r} within {timeout:.0f} s")
            line = self.t.read_line(timeout=left)
            if line is None:
                if self.t.closed:
                    raise ConsoleError(f"connection closed during {cmd!r}")
                continue
            if line.startswith("@T "):
                rec = json.loads(line[3:])
                if rec.get("done"):
                    reply.done = rec
                    return reply
                reply.records.append(rec)
            elif not line.startswith("@F "):
                reply.logs.append(line)

    def close(self) -> None:
        self.t.close()
