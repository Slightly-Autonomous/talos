"""The TL-SN-002 flash + provision + self-test entry point (talos_bench.flash)."""

import contextlib
import io
import json
import os
import pathlib
import tempfile
import unittest

from talos_bench import flash

BENCH_NODE = os.environ.get("TALOS_BENCH_NODE")


class FlashCommand(unittest.TestCase):
    def test_prefers_esptool_with_the_builds_flash_args(self):
        with tempfile.TemporaryDirectory() as d:
            build = pathlib.Path(d) / "build"
            build.mkdir()
            (build / "flash_args").write_text("0x0 bootloader.bin\n")
            cmd = flash.flash_command(build, "/dev/ttyACM0", which=lambda t: f"/bin/{t}")
            self.assertEqual(cmd[:4], ["/bin/esptool.py", "--chip", "esp32h2", "-p"])
            self.assertEqual(cmd[-1], f"@{build / 'flash_args'}")

    def test_falls_back_to_idf_py_then_to_nothing(self):
        build = pathlib.Path("/nonexistent/build")
        idf = flash.flash_command(build, "/dev/ttyACM0",
                                  which=lambda t: "/bin/idf.py" if t == "idf.py" else None)
        self.assertEqual(idf[0], "/bin/idf.py")
        self.assertIn("flash", idf)
        self.assertIsNone(flash.flash_command(build, "/dev/ttyACM0", which=lambda t: None))


@unittest.skipUnless(BENCH_NODE, "set TALOS_BENCH_NODE (bazel build //firmware/sim:bench_node)")
class FlashOnTheVirtualBoard(unittest.TestCase):
    def run_flash(self, *extra):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = flash.main(["--unit-serial", "SN2-A-0007", *extra, "--exec", BENCH_NODE,
                               "--board=TL-SN-002", "--no-board-id"])
        lines = [json.loads(line) for line in out.getvalue().splitlines() if line.startswith("{")]
        return code, lines

    def test_blank_board_gets_its_identity_and_passes_the_self_test(self):
        code, lines = self.run_flash()
        steps = {line["step"]: line for line in lines if "step" in line}
        self.assertEqual(steps["flash"]["verdict"], "skipped")  # no port: nothing to flash, said so
        # The console is read-only with the lid closed: the tool opened it before the identity.
        self.assertEqual(steps["lid.open"]["verdict"], "pass")
        self.assertEqual(steps["identity"]["verdict"], "pass")
        self.assertEqual(steps["selftest.S-M20"]["verdict"], "pass")  # closed again: locked
        self.assertEqual(steps["selftest.S-M21"]["verdict"], "pass")
        self.assertEqual(steps["identity"]["detail"],
                         {"board": "TL-SN-002", "rev": "A", "serial": "SN2-A-0007"})
        self.assertEqual(steps["selftest.N-01.mag"]["verdict"], "pass")
        self.assertEqual(lines[-1]["summary"]["failed"], [])
        self.assertEqual(code, 0)

    def test_identity_for_another_board_is_refused_not_burned(self):
        # Red team: --board TL-SN-001 against a node running the TL-SN-002 table (e.g. the
        # flash step skipped and another image left in place) must not write a permanent label.
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = flash.main(["--unit-serial", "SN-A-0007", "--board", "TL-SN-001", "--exec",
                               BENCH_NODE, "--board=TL-SN-002", "--no-board-id"])
        lines = [json.loads(line) for line in out.getvalue().splitlines() if line.startswith("{")]
        steps = {line["step"]: line for line in lines if "step" in line}
        self.assertEqual(steps["identity"]["verdict"], "fail")
        self.assertIn("another board", steps["identity"]["diagnosis"])
        self.assertNotEqual(code, 0)


if __name__ == "__main__":
    unittest.main()
