"""Start a throwaway development hub for bench and HIL runs.

Copies deploy/config/hub.dev.yaml with free ports, absolute paths and a temporary journal,
so a run never touches the repository's .data/ or collides with a hub already running.
The dev config's tokens and device keys are public and insecure on purpose.
"""

from __future__ import annotations

import json
import os
import pathlib
import re
import socket
import subprocess
import tempfile
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[3]
DEV_CONFIG = ROOT / "deploy/config/hub.dev.yaml"
DEV_TOKEN = "dev-owner-token"  # deploy/README.md
DEV_KEYS = {"node-07": "000102030405060708090a0b0c0d0e0f",
            "node-12": "101112131415161718191a1b1c1d1e1f"}


def free_port(kind: int = socket.SOCK_STREAM) -> int:
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class DevHub:
    def __init__(self, hub_bin: str):
        self.dir = pathlib.Path(tempfile.mkdtemp(prefix="talos-hub-"))
        self.api_port = free_port()
        self.coap_port = free_port(socket.SOCK_DGRAM)
        text = DEV_CONFIG.read_text()
        text = re.sub(r"^port: \d+", f"port: {self.api_port}", text, count=1, flags=re.M)
        text = re.sub(r"(coap:\n(?:  .*\n)*?  port: )\d+", rf"\g<1>{self.coap_port}", text)
        for key in ("property_file", "device_keys_file"):
            text = re.sub(rf"({key}: )(\S+)", lambda m: m.group(1) + str((DEV_CONFIG.parent / m.group(2)).resolve()), text)
        cfg = self.dir / "hub.yaml"
        cfg.write_text(text)
        env = dict(os.environ, TALOS_SQLITE_PATH=str(self.dir / "journal.db"))
        self.proc = subprocess.Popen([hub_bin, "--config", str(cfg)], cwd=self.dir, env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self._wait_ready()

    def _wait_ready(self, timeout: float = 20.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"hub exited: {self.proc.stdout.read() if self.proc.stdout else ''}")
            try:
                self.get("/v1/health")
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("hub did not come up")

    def get(self, path: str):
        req = urllib.request.Request(f"http://127.0.0.1:{self.api_port}{path}",
                                     headers={"Authorization": f"Bearer {DEV_TOKEN}"})
        with urllib.request.urlopen(req, timeout=2) as r:
            return json.loads(r.read() or b"null")

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
