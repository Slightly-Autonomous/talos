"""The agent's pin table must match the board manifest (the single source for TL-VN-001 pins)."""
import pathlib
import re
import unittest

from talos_vision import board

ROOT = pathlib.Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "contracts" / "manifests" / "boards" / "vision_node_pi_zero2w.yaml"


class BoardMatchesManifest(unittest.TestCase):
    def setUp(self):
        self.text = MANIFEST.read_text()

    def test_identity_and_provisional(self):
        self.assertIn(f"id: {board.BOARD_ID}", self.text)
        self.assertIn(f"revision: {board.BOARD_REVISION}", self.text)
        self.assertIn(f"board_family: {board.BOARD_FAMILY}", self.text)
        self.assertIn("provisional: true", self.text)

    def test_tamper_pin(self):
        note = next(l for l in self.text.splitlines() if "Lid tamper" in l)
        self.assertIn(f"GPIO{board.TAMPER_GPIO} (header pin {board.TAMPER_HEADER_PIN})", note)
        self.assertIn("high = lid open", note)
        self.assertEqual(board.TAMPER_OPEN_LEVEL, 1)

    def test_capabilities(self):
        caps = re.search(r"board_capabilities: \[(.*)\]", self.text).group(1)
        self.assertEqual(tuple(c.strip() for c in caps.split(",")), board.CAPABILITIES)


if __name__ == "__main__":
    unittest.main()
