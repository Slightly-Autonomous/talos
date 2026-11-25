import argparse
import hashlib
import io
import json
import pathlib
import sys
import tarfile
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "provision"))
import vision_node as prov  # noqa: E402


def fake_release(d: pathlib.Path, name="mediamtx_v1.21.4_linux_arm64.tar.gz"):
    archive = d / name
    with tarfile.open(archive, "w:gz") as t:
        data = b"\x7fELF fake mediamtx"
        info = tarfile.TarInfo("mediamtx")
        info.size = len(data)
        t.addfile(info, io.BytesIO(data))
    (d / "checksums.sha256").write_text(
        f"{hashlib.sha256(archive.read_bytes()).hexdigest()}  {name}\n")
    return archive


def args(boot, archive, **kw):
    base = dict(boot=str(boot), device_id="cam_patio", property_id="house-01",
                hub_url="http://hub.local:8080", mediamtx=str(archive),
                checksums=str(archive.parent / "checksums.sha256"), token=None, rtsp_password=None,
                hostname=None, frigate_name="patio", ntp="192.168.1.1", overlay=True,
                detect_stream=False)
    base.update(kw)
    return argparse.Namespace(**base)


class Provision(unittest.TestCase):
    def setUp(self):
        self.d = pathlib.Path(tempfile.mkdtemp())
        self.boot = self.d / "bootfs"
        self.boot.mkdir()
        (self.boot / "cmdline.txt").write_text("console=serial0,115200 root=PARTUUID=abc rootwait\n")
        (self.boot / "config.txt").write_text("dtparam=audio=on\n")
        self.archive = fake_release(self.d)

    def test_card_layout_secrets_and_boot_files(self):
        out = prov.provision(args(self.boot, self.archive))
        t = self.boot / "talos"
        self.assertTrue((t / "agent" / "talos_vision" / "agent.py").exists())
        self.assertEqual((t / "mediamtx").read_bytes(), b"\x7fELF fake mediamtx")
        cfg = json.loads((t / "vision.json").read_text())
        self.assertEqual(cfg["deviceId"], "cam_patio")
        token = (t / "device.token").read_text().strip()
        self.assertNotIn(token, json.dumps(cfg))  # the token lives in its own 0640 file
        self.assertEqual(out["hub_device"]["token_sha256"], hashlib.sha256(token.encode()).hexdigest())
        mtx = (t / "mediamtx.yml").read_text()
        self.assertIn(out["secrets"]["rtsp_password"], mtx)
        self.assertNotIn("@RTSP_PASSWORD@", mtx)
        self.assertIn("rpiCameraTuningFile", mtx)
        self.assertNotIn("\n  detect:", mtx)  # second stream off by default
        self.assertTrue((t / "overlay").exists())
        self.assertEqual((t / "ntp").read_text().strip(), "192.168.1.1")
        self.assertIn("verified", out["mediamtx"])
        self.assertIn("patio", out["frigate_camera"])
        cmdline = (self.boot / "cmdline.txt").read_text()
        self.assertTrue(cmdline.startswith("console=serial0,115200 root=PARTUUID=abc rootwait"))
        self.assertIn("systemd.run=/boot/firmware/firstrun.sh", cmdline)
        self.assertEqual(cmdline.count("\n"), 1)
        config = (self.boot / "config.txt").read_text()
        for line in ("dtoverlay=disable-wifi", "dtoverlay=disable-bt", "dtoverlay=dwc2,dr_mode=host",
                     "dtparam=watchdog=on"):
            self.assertIn(line, config)
        self.assertTrue((self.boot / "firstrun.sh").exists())

    def test_running_twice_does_not_duplicate_boot_settings(self):
        prov.provision(args(self.boot, self.archive))
        prov.provision(args(self.boot, self.archive, token="t" * 40, rtsp_password="p" * 20))
        self.assertEqual((self.boot / "config.txt").read_text().count("disable-wifi"), 1)
        self.assertEqual((self.boot / "cmdline.txt").read_text().count("systemd.run="), 1)
        self.assertEqual((self.boot / "talos" / "device.token").read_text().strip(), "t" * 40)

    def test_detect_stream_is_uncommented(self):
        prov.provision(args(self.boot, self.archive, detect_stream=True))
        mtx = (self.boot / "talos" / "mediamtx.yml").read_text()
        self.assertIn("\n  detect:\n    source: rpiCamera\n", mtx)
        self.assertIn("rpiCameraSecondary: true", mtx)

    def test_refusals(self):
        with self.assertRaises(ValueError):
            prov.provision(args(self.d, self.archive))  # not a boot partition
        with self.assertRaises(ValueError):
            prov.provision(args(self.boot, self.archive, device_id="cam/patio"))
        with self.assertRaises(ValueError):
            prov.provision(args(self.boot, self.archive, rtsp_password="short"))
        (self.d / "checksums.sha256").write_text("0" * 64 + "  " + self.archive.name + "\n")
        with self.assertRaises(ValueError):
            prov.provision(args(self.boot, self.archive))  # checksum mismatch
        armv7 = fake_release(self.d, "mediamtx_v1.21.4_linux_armv7.tar.gz")
        with self.assertRaises(ValueError):
            prov.provision(args(self.boot, armv7))

    def test_unverified_archive_is_flagged(self):
        out = prov.provision(args(self.boot, self.archive, checksums=None))
        self.assertIn("UNVERIFIED", out["mediamtx"])


if __name__ == "__main__":
    unittest.main()
