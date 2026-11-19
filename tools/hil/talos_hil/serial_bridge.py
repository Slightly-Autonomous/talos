"""Relay a node's console datagrams to the hub, and share its console.

    python3 -m talos_hil.serial_bridge --port /dev/cu.usbmodem1101 --hub 127.0.0.1:5683 --console 7770

Until the Thread radio carries CoAP, a node sends its CoAP datagrams over USB-serial-JTAG as
"@F <base64>" lines (firmware/portable/serial_link.h). This bridge owns the serial port:
- "@F" lines from the node go to the hub's CoAP UDP port; UDP replies go back as "@F" lines.
- Every other line (diag "@T" replies, logs) goes to the clients on the --console TCP port,
  and lines from those clients go to the node. talos_hil.console.TcpTransport connects there.
The hub then runs its real ingress (HMAC, dedupe, ACKs) against the real board.
"""

from __future__ import annotations

import argparse
import base64
import socket
import sys
import threading

from .console import ProcessTransport, SerialTransport, Transport


class Bridge:
    def __init__(self, node: Transport, hub: tuple[str, int], console_port: int | None = None,
                 log=None):
        self.node = node
        self.hub = hub
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(("127.0.0.1", 0))
        self.udp.settimeout(0.2)
        self.clients: list[socket.socket] = []
        self.lock = threading.Lock()
        self.log = log
        self.frames_up = self.frames_down = self.bad_frames = 0
        self.running = True
        self.server: socket.socket | None = None
        if console_port is not None:
            self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.server.bind(("127.0.0.1", console_port))
            self.server.listen(4)
        self.console_port = self.server.getsockname()[1] if self.server else None

    def start(self) -> "Bridge":
        threading.Thread(target=self._from_node, daemon=True).start()
        threading.Thread(target=self._from_hub, daemon=True).start()
        if self.server:
            threading.Thread(target=self._accept, daemon=True).start()
        return self

    def stop(self) -> None:
        self.running = False
        if self.server:
            self.server.close()
        with self.lock:
            for c in self.clients:
                c.close()
        self.udp.close()

    def _from_node(self) -> None:
        while self.running:
            line = self.node.read_line(timeout=0.2)
            if line is None:
                if self.node.closed:
                    return
                continue
            if line.startswith("@F "):
                try:
                    data = base64.b64decode(line[3:], validate=True)
                except ValueError:
                    self.bad_frames += 1
                    continue
                self.udp.sendto(data, self.hub)
                self.frames_up += 1
                continue
            if self.log:
                self.log(line)
            self._to_clients(line + "\n")

    def _from_hub(self) -> None:
        while self.running:
            try:
                data, _ = self.udp.recvfrom(4096)
            except (socket.timeout, OSError):
                continue
            self.node.write_line("@F " + base64.b64encode(data).decode())
            self.frames_down += 1

    def _to_clients(self, text: str) -> None:
        with self.lock:
            for c in list(self.clients):
                try:
                    c.sendall(text.encode())
                except OSError:
                    self.clients.remove(c)

    def _accept(self) -> None:
        assert self.server
        while self.running:
            try:
                conn, _ = self.server.accept()
            except OSError:
                return
            with self.lock:
                self.clients.append(conn)
            threading.Thread(target=self._from_client, args=(conn,), daemon=True).start()

    def _from_client(self, conn: socket.socket) -> None:
        buf = b""
        while self.running:
            try:
                chunk = conn.recv(4096)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").strip()
                if line and not line.startswith("@F "):  # clients may not inject datagrams
                    self.node.write_line(line)
        with self.lock:
            if conn in self.clients:
                self.clients.remove(conn)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="talos_hil.serial_bridge", description=__doc__.split("\n")[0])
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--port", help="the node's serial port, e.g. /dev/cu.usbmodem1101")
    src.add_argument("--exec", nargs=argparse.REMAINDER, help="run a virtual node (bench_node ...)")
    p.add_argument("--hub", default="127.0.0.1:5683", help="hub CoAP host:port")
    p.add_argument("--console", type=int, default=7770, help="TCP port for diag clients")
    args = p.parse_args(argv)
    host, port = args.hub.rsplit(":", 1)
    node = SerialTransport(args.port) if args.port else ProcessTransport(args.exec)
    bridge = Bridge(node, (host, int(port)), args.console, log=lambda l: print(l, flush=True)).start()
    print(f"bridging to hub {args.hub}; diag console on 127.0.0.1:{bridge.console_port}",
          file=sys.stderr, flush=True)
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        pass
    finally:
        bridge.stop()
        node.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
