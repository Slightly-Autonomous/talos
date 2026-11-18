"""Real USB access through hidapi (`pip install hidapi`). Only imported when hardware is used."""

from __future__ import annotations

from dataclasses import dataclass

from .mcp2221 import REPORT

TIMEOUT_MS = 1000


@dataclass(frozen=True)
class HidInfo:
    path: bytes
    vid: int
    pid: int
    product: str


def _hid():
    try:
        import hid  # type: ignore
    except ImportError as e:
        raise SystemExit("hidapi is not installed: pip install hidapi") from e
    return hid


def enumerate_bridges(vid: int, pid: int) -> list[HidInfo]:
    return [HidInfo(d["path"], d["vendor_id"], d["product_id"], d.get("product_string") or "")
            for d in _hid().enumerate(vid, pid)]


class HidDevice:
    def __init__(self, path: bytes):
        self._dev = _hid().device()
        self._dev.open_path(path)

    def write(self, report: bytes) -> None:
        # hidapi wants the report ID first; the MCP2221A uses unnumbered reports (ID 0).
        if self._dev.write(b"\x00" + report) < 0:
            raise OSError("HID write failed")

    def read(self) -> bytes:
        data = bytes(self._dev.read(REPORT, TIMEOUT_MS))
        if len(data) < REPORT:
            raise TimeoutError("no HID response from the MCP2221A")
        return data

    def close(self) -> None:
        self._dev.close()
