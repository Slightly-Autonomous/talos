import contextlib
import io
import os
import tempfile
import unittest
from unittest import mock

from talos_dbg import cli
from talos_dbg.mcp2221 import Mcp2221
from tests.fake_mcp2221 import FakeMcp2221, IdEeprom


class Dev:
    def __init__(self, fake):
        self.fake = fake

    def close(self):
        pass


class Cli(unittest.TestCase):
    def setUp(self):
        self.fake = FakeMcp2221(serial="SER001")
        self.fake.card[0x50] = IdEeprom(0x0301, 2)
        os.environ["TALOS_DBG_LOCK_DIR"] = tempfile.mkdtemp()
        patcher = mock.patch.object(
            cli, "_open_all", lambda vid, pid: iter([(None, Dev(self.fake), Mcp2221(self.fake, sleep=lambda s: None))]))
        patcher.start()
        self.addCleanup(patcher.stop)
        sleep = mock.patch("talos_dbg.adapter.time.sleep", lambda s: None)
        sleep.start()
        self.addCleanup(sleep.stop)

    def run_cli(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cli.main(list(argv))
        return code, out.getvalue() + err.getvalue()

    def test_bring_up_flow(self):
        code, text = self.run_cli("id")
        self.assertEqual(code, 2)
        self.assertIn("not configured", text)
        code, text = self.run_cli("configure")
        self.assertEqual(code, 2)
        self.assertIn("no card is seated", text)
        self.assertEqual(self.run_cli("configure", "--no-card-seated")[0], 0)
        self.assertEqual(self.run_cli("power", "aon", "on")[0], 0)
        code, text = self.run_cli("id")
        self.assertEqual(code, 0)
        self.assertIn("environment_sht40_v1", text)
        code, text = self.run_cli("status")
        self.assertIn("AON=on", text)
        self.assertEqual(self.run_cli("power", "all", "off")[0], 0)
        self.assertFalse(self.fake.aon_on)


if __name__ == "__main__":
    unittest.main()
