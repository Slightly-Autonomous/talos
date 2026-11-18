"""Find adapters and hold one exclusively, keyed by the MCP2221A factory serial number.

The factory serial is burned in and readable over HID, so it identifies an adapter whatever
USB port it is on, without rewriting the chip's USB settings. The lock is an advisory flock on
a per-serial file: two processes (two agents, or a person and an agent) cannot drive the same
adapter at once. The lock dies with the process, so a crash never leaves it held.
"""

from __future__ import annotations

import fcntl
import os
import pathlib
from dataclasses import dataclass

LOCK_DIR = pathlib.Path(os.environ.get("TALOS_DBG_LOCK_DIR",
                                       pathlib.Path.home() / ".cache" / "talos-dbg" / "locks"))


class AdapterBusy(Exception):
    pass


@dataclass
class AdapterLock:
    serial: str
    _fd: int

    def release(self) -> None:
        if self._fd >= 0:
            fcntl.flock(self._fd, fcntl.LOCK_UN)
            os.close(self._fd)
            self._fd = -1

    def __enter__(self) -> "AdapterLock":
        return self

    def __exit__(self, *exc) -> None:
        self.release()


def lock(serial: str, lock_dir: pathlib.Path | None = None) -> AdapterLock:
    if not serial or not serial.replace("-", "").isalnum():
        raise ValueError(f"unusable adapter serial {serial!r}")
    d = lock_dir or LOCK_DIR
    d.mkdir(parents=True, exist_ok=True)
    fd = os.open(d / f"{serial}.lock", os.O_RDWR | os.O_CREAT, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(fd)
        raise AdapterBusy(f"adapter {serial} is in use by another process") from None
    os.ftruncate(fd, 0)
    os.write(fd, f"{os.getpid()}\n".encode())
    return AdapterLock(serial, fd)
