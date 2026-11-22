"""program-id --node (practices review D-09): board, revision and serial into talos_id."""

import contextlib
import io
import os
import pathlib
import sys
import unittest
from unittest import mock

from talos_dbg import cli, node_id

ROOT = pathlib.Path(__file__).resolve().parents[3]
BENCH_NODE = os.environ.get("TALOS_BENCH_NODE")


class Reply:
    def __init__(self, records, done):
        self.records, self.done = records, done

    @property
    def ok(self):
        return bool(self.done.get("ok"))

    def record(self, t):
        return next((r for r in self.records if r.get("t") == t), None)


class FakeNode:
    """The firmware's board-id/id commands (firmware/portable/diag.cc): write-once."""

    def __init__(self, info=None, corrupt_readback=False, locked=False):
        self.info = info
        self.corrupt_readback = corrupt_readback
        self.locked = locked  # TL-SN-002 with the lid closed: read-only console
        self.sent = []
        self.closed = False

    def command(self, cmd, timeout=10.0):
        self.sent.append(cmd)
        words = cmd.split()
        if self.locked and words[0] == "board-id" and len(words) > 1:
            return Reply([], {"cmd": "board-id", "done": True, "ok": False,
                              "err": "locked: open the enclosure"})
        if words[0] == "board-id":
            err = None
            want = tuple(words[1:4])
            if self.info and self.info != want:
                err = "board-id already programmed (write-once)"
            elif not self.info:
                self.info = want
            rec = {"t": "board_id", "programmed": bool(self.info)}
            if self.info:
                rec.update(board=self.info[0], rev=self.info[1], serial=self.info[2])
            done = {"cmd": "board-id", "done": True, "ok": err is None}
            if err:
                done["err"] = err
            return Reply([rec], done)
        if words[0] == "id":
            b, r, s = self.info or ("", "", "")
            if self.corrupt_readback:
                s = s + "x"
            return Reply([{"t": "id", "board": b, "rev": r, "serial": s, "board_ok": True}],
                         {"cmd": "id", "done": True, "ok": True})
        raise AssertionError(cmd)

    def close(self):
        self.closed = True


class Program(unittest.TestCase):
    def test_writes_tl_sn_001_rev_a_and_verifies(self):
        node = FakeNode()
        rec = node_id.program(node, "TL-SN-001", "A", "SN-A-0007")
        self.assertEqual((rec["board"], rec["rev"], rec["serial"]), ("TL-SN-001", "A", "SN-A-0007"))
        self.assertEqual(node.sent, ["board-id TL-SN-001 A SN-A-0007", "id"])

    def test_same_identity_again_is_fine(self):
        node = FakeNode(("TL-SN-001", "A", "SN-A-0007"))
        node_id.program(node, "TL-SN-001", "A", "SN-A-0007")

    def test_a_different_identity_is_refused_and_named(self):
        node = FakeNode(("TL-SN-001", "A", "SN-A-0001"))
        with self.assertRaisesRegex(node_id.NodeIdError, "write-once.*SN-A-0001"):
            node_id.program(node, "TL-SN-001", "A", "SN-A-0007")

    def test_locked_console_says_open_the_lid(self):
        node = FakeNode(locked=True)
        with self.assertRaisesRegex(node_id.NodeIdError, "read-only while its enclosure is closed.*open the lid"):
            node_id.program(node, "TL-SN-002", "A", "SN2-A-0007")
        self.assertIsNone(node.info)

    def test_readback_mismatch_fails(self):
        with self.assertRaisesRegex(node_id.NodeIdError, "read-back mismatch"):
            node_id.program(FakeNode(corrupt_readback=True), "TL-SN-001", "A", "SN-A-0007")

    def test_unknown_board_revision_or_bad_serial_never_reach_the_node(self):
        for args in (("TL-XX-001", "A", "S1"), ("TL-SN-001", "B", "S1"), ("TL-SN-001", "A", "bad/serial"),
                     ("TL-SN-001", "A", ""), ("TL-SN-001", "A", "x" * 24)):
            node = FakeNode()
            with self.assertRaises(node_id.NodeIdError, msg=args):
                node_id.program(node, *args)
            self.assertEqual(node.sent, [])

    def test_bench_runner_expects_what_the_tool_writes(self):
        sys.path.insert(0, str(ROOT / "tools/bench"))
        sys.path.insert(0, str(ROOT / "tools/hil"))
        from talos_bench.runner import BOARD  # noqa: PLC0415

        self.assertEqual((BOARD["board"], BOARD["rev"]), (node_id.DEFAULT_BOARD, node_id.DEFAULT_REVISION))
        self.assertIn(BOARD["rev"], node_id.BOARDS[BOARD["board"]])
        manifest = (ROOT / "contracts/manifests/boards/sensor_node_proto_a.yaml").read_text()
        self.assertIn(f"board_family: {node_id.DEFAULT_BOARD}", manifest)


class Cli(unittest.TestCase):
    def run_cli(self, *argv, node=None):
        node = node or FakeNode()
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(node_id, "open_console", lambda target: node), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cli.main(list(argv))
        return code, out.getvalue() + err.getvalue(), node

    def test_node_mode_defaults_to_tl_sn_001_rev_a(self):
        code, text, node = self.run_cli("program-id", "--node", "/dev/null", "--unit-serial", "SN-A-0009")
        self.assertEqual(code, 0, text)
        self.assertIn("board TL-SN-001 rev A serial SN-A-0009 programmed and verified", text)
        self.assertTrue(node.closed)

    def test_node_mode_needs_a_serial(self):
        with self.assertRaises(SystemExit):
            self.run_cli("program-id", "--node", "/dev/null")

    def test_refusal_exits_2(self):
        code, text, _ = self.run_cli("program-id", "--node", "/dev/null", "--unit-serial", "SN-2",
                                     node=FakeNode(("TL-SN-001", "A", "SN-1")))
        self.assertEqual(code, 2)
        self.assertIn("write-once", text)


@unittest.skipUnless(BENCH_NODE, "set TALOS_BENCH_NODE (bazel build //firmware/sim:bench_node)")
class AgainstTheVirtualNode(unittest.TestCase):
    def test_program_then_refuse_a_change(self):
        sys.path.insert(0, str(ROOT / "tools/hil"))
        from talos_hil.console import Console, ProcessTransport  # noqa: PLC0415

        console = Console(ProcessTransport([BENCH_NODE, "--no-board-id"]))
        try:
            rec = node_id.program(console, "TL-SN-001", "A", "SN-A-0042")
            self.assertEqual(rec["serial"], "SN-A-0042")
            with self.assertRaisesRegex(node_id.NodeIdError, "write-once"):
                node_id.program(console, "TL-SN-001", "A", "SN-A-0043")
        finally:
            console.close()

    def test_tl_sn_002_needs_the_lid_open(self):
        sys.path.insert(0, str(ROOT / "tools/hil"))
        from talos_hil.console import Console, ProcessTransport  # noqa: PLC0415

        console = Console(ProcessTransport([BENCH_NODE, "--board=TL-SN-002", "--no-board-id"]))
        try:
            console.command("sim run 300")  # past the tamper debounce: the lid reads closed
            with self.assertRaisesRegex(node_id.NodeIdError, "open the lid"):
                node_id.program(console, "TL-SN-002", "A", "SN2-A-0042")
            console.command("sim fault lid_open")
            console.command("sim run 300")
            rec = node_id.program(console, "TL-SN-002", "A", "SN2-A-0042")
            self.assertEqual(rec["serial"], "SN2-A-0042")
        finally:
            console.close()


if __name__ == "__main__":
    unittest.main()
