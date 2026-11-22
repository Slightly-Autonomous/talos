"""Board identity on a sensor node (practices review D-09).

No GPIO or ADC is free for a revision strap, so the board name, revision and unit serial are
written once, at program time, into the node's permanent NVS partition (`talos_id`, which
firmware never erases) through its diag console (`board-id <board> <revision> <serial>`).
The firmware refuses to change a programmed identity; this tool then reads it back with `id`
and checks every field. tools/bench (S-M16) checks the same fields on every bench run.

    python3 -m talos_dbg program-id --node /dev/cu.usbmodem1101 --unit-serial SN-A-0007
"""

from __future__ import annotations

import pathlib
import re
import sys

# Boards and revisions this tool may write. Keep in step with tools/bench (BOARD, BOARD_SN002)
# and the board manifests' board_family (sensor_node_proto_a.yaml, sensor_node_v2.yaml).
# TL-SN-002 (ADR 0019) is the current node; tools/flash defaults to it.
BOARDS = {"TL-SN-001": ("A",), "TL-SN-002": ("A",)}
DEFAULT_BOARD, DEFAULT_REVISION = "TL-SN-001", "A"
_FIELD = re.compile(r"^[A-Za-z0-9._-]+$")
_MAX = {"board": 15, "revision": 3, "serial": 23}  # firmware/portable/hal.h BoardInfo


class NodeIdError(Exception):
    pass


def validate(board: str, revision: str, serial: str) -> None:
    if board not in BOARDS:
        raise NodeIdError(f"unknown board {board!r}; known: {sorted(BOARDS)}")
    if revision not in BOARDS[board]:
        raise NodeIdError(f"{board} has no revision {revision!r}; known: {list(BOARDS[board])}")
    for name, value in (("board", board), ("revision", revision), ("serial", serial)):
        if not value or len(value) > _MAX[name] or not _FIELD.match(value):
            raise NodeIdError(f"{name} {value!r}: 1-{_MAX[name]} characters of A-Z a-z 0-9 . _ -")


def program(console, board: str, revision: str, serial: str) -> dict:
    """Writes the identity (or confirms it is already exactly this one) and verifies it."""
    validate(board, revision, serial)
    reply = console.command(f"board-id {board} {revision} {serial}")
    if not reply.ok:
        have = reply.record("board_id") or {}
        detail = (f" (node has {have.get('board')} rev {have.get('rev')} serial {have.get('serial')})"
                  if have.get("programmed") else "")
        raise NodeIdError(f"node refused board-id: {reply.done.get('err', 'no reason given')}{detail}")
    rec = console.command("id").record("id") or {}
    got = (rec.get("board"), rec.get("rev"), rec.get("serial"))
    if got != (board, revision, serial):
        raise NodeIdError(f"read-back mismatch: wrote {(board, revision, serial)}, node reports {got}")
    return rec


def open_console(target: str):
    """A diag console on a serial port path or a serial_bridge host:port."""
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "hil"))
    from talos_hil.console import Console, SerialTransport, TcpTransport  # noqa: PLC0415

    if ":" in target and not target.startswith("/"):
        host, port = target.rsplit(":", 1)
        return Console(TcpTransport(host, int(port)))
    return Console(SerialTransport(target))
