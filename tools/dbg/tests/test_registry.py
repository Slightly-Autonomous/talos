import pathlib
import tempfile
import unittest

from talos_dbg.registry import AdapterBusy, lock


class Locks(unittest.TestCase):
    def setUp(self):
        self.dir = pathlib.Path(tempfile.mkdtemp())

    def test_one_holder_per_adapter(self):
        with lock("ABC123", self.dir):
            with self.assertRaises(AdapterBusy):
                lock("ABC123", self.dir)
            with lock("OTHER1", self.dir):
                pass
        lock("ABC123", self.dir).release()

    def test_rejects_path_like_serials(self):
        for bad in ("", "../x", "a/b"):
            with self.assertRaises(ValueError):
                lock(bad, self.dir)


if __name__ == "__main__":
    unittest.main()
