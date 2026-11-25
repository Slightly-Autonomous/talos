"""Provision a TL-VN-001 Vision Node SD card (ADR 0020).

1. Flash the official Raspberry Pi OS Lite (64-bit) image with Raspberry Pi Imager. In its settings,
   set the user and SSH key only; leave Wi-Fi empty (the camera is Ethernet-only).
2. Re-insert the card so its boot partition mounts (macOS: /Volumes/bootfs), then:

    python3 tools/provision/vision_node.py --boot /Volumes/bootfs --device-id cam_patio \\
        --hub-url http://hub.local:8080 --mediamtx mediamtx_v1.21.4_linux_arm64.tar.gz \\
        [--checksums checksums.sha256] [--ntp 192.168.1.1] [--overlay] [--detect-stream]

It copies the agent, MediaMTX and the systemd units onto the boot partition, and renders the stream
config with a per-camera RTSP password. It writes the device token and a first-boot script that
installs everything, and sets config.txt: Wi-Fi and Bluetooth off, USB host mode for the PoE HAT's
Ethernet, and the hardware watchdog.

It prints, once, what the hub and Frigate need: the token's SHA-256 for the hub's `devices` list,
and the camera map entry with the RTSP password. Nothing secret is written into the repository. The
token and password are generated here unless you pass them (--token, --rtsp-password).

MediaMTX comes from https://github.com/bluenviron/mediamtx/releases (linux_arm64). Pass that release's
checksums.sha256 with --checksums, or the archive is used unverified and the tool says so.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import secrets
import shutil
import sys
import tarfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
NODE = ROOT / "firmware" / "vision_node"
ID = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
CONFIG_BEGIN = "# --- Talos TL-VN-001 (tools/provision/vision_node.py) ---"
CONFIG_END = "# --- end Talos ---"
CONFIG_BLOCK = f"""{CONFIG_BEGIN}
[all]
camera_auto_detect=1
dtoverlay=disable-wifi
dtoverlay=disable-bt
# The PoE HAT's Ethernet and USB hub hang off the Zero's USB port in host mode (Waveshare wiki).
dtoverlay=dwc2,dr_mode=host
dtparam=watchdog=on
{CONFIG_END}
"""
FIRSTRUN_ARGS = ("systemd.run=/boot/firmware/firstrun.sh systemd.run_success_action=reboot "
                 "systemd.unit=kernel-command-line.target")


def render_mediamtx(password: str, detect_stream: bool) -> str:
    text = (NODE / "system" / "mediamtx.yml.template").read_text()
    if not re.fullmatch(r"[A-Za-z0-9_-]{16,128}", password):
        raise ValueError("RTSP password: 16-128 of [A-Za-z0-9_-]")
    text = text.replace("@RTSP_PASSWORD@", password)
    if detect_stream:
        block = re.search(r"  # @DETECT_STREAM_BEGIN@\n(.*?)  # @DETECT_STREAM_END@\n", text, re.S)
        assert block
        lines = [l.replace("  # ", "  ", 1) for l in block.group(1).splitlines(keepends=True)]
        text = text.replace(block.group(0), "".join(lines))
    return text


def extract_mediamtx(archive: pathlib.Path, checksums: pathlib.Path | None) -> tuple[bytes, str]:
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    note = "UNVERIFIED (no --checksums)"
    if checksums:
        listed = {}
        for line in checksums.read_text().splitlines():
            parts = line.split()
            if len(parts) == 2:
                listed[parts[1].lstrip("*")] = parts[0].lower()
        if listed.get(archive.name) != digest:
            raise ValueError(f"{archive.name}: sha256 {digest} is not in {checksums.name}")
        note = "verified against " + checksums.name
    if "linux_arm64" not in archive.name:
        raise ValueError("use the linux_arm64 MediaMTX archive (64-bit Raspberry Pi OS)")
    with tarfile.open(archive) as t:
        member = next((m for m in t.getmembers() if m.isfile() and pathlib.PurePath(m.name).name == "mediamtx"),
                      None)
        if member is None:
            raise ValueError("no mediamtx binary in the archive")
        f = t.extractfile(member)
        assert f
        return f.read(), f"sha256 {digest}, {note}"


def set_config_txt(path: pathlib.Path) -> None:
    text = path.read_text()
    text = re.sub(re.escape(CONFIG_BEGIN) + r".*?" + re.escape(CONFIG_END) + r"\n?", "", text, flags=re.S)
    path.write_text(text.rstrip("\n") + "\n\n" + CONFIG_BLOCK)


def set_cmdline(path: pathlib.Path) -> None:
    line = path.read_text().strip()
    for arg in FIRSTRUN_ARGS.split():
        key = arg.split("=")[0]
        line = re.sub(rf"\s{re.escape(key)}=\S+", "", line)
    line = re.sub(r"\ssystemd\.unit=kernel-command-line\.target", "", line)
    path.write_text(line + " " + FIRSTRUN_ARGS + "\n")


def provision(a: argparse.Namespace) -> dict:
    boot = pathlib.Path(a.boot)
    for f in ("cmdline.txt", "config.txt"):
        if not (boot / f).is_file():
            raise ValueError(f"{boot} is not a Raspberry Pi boot partition (no {f})")
    if not ID.match(a.device_id):
        raise ValueError("device id: 1-64 of [A-Za-z0-9_-]")
    token = a.token or secrets.token_urlsafe(32)
    password = a.rtsp_password or secrets.token_urlsafe(24)
    binary, mtx_note = extract_mediamtx(pathlib.Path(a.mediamtx),
                                        pathlib.Path(a.checksums) if a.checksums else None)
    t = boot / "talos"
    if t.exists():
        shutil.rmtree(t)
    (t / "agent").mkdir(parents=True)
    shutil.copytree(NODE / "talos_vision", t / "agent" / "talos_vision",
                    ignore=shutil.ignore_patterns("__pycache__"))
    (t / "mediamtx").write_bytes(binary)
    (t / "mediamtx.yml").write_text(render_mediamtx(password, a.detect_stream))
    (t / "vision.json").write_text(json.dumps({
        "deviceId": a.device_id, "propertyId": a.property_id, "hubUrl": a.hub_url,
        "tokenFile": "/etc/talos/device.token"}, indent=2) + "\n")
    (t / "device.token").write_text(token + "\n")
    hostname = a.hostname or a.device_id.replace("_", "-")
    (t / "hostname").write_text(hostname + "\n")
    (t / "ntp").write_text((a.ntp or "") + "\n" if a.ntp else "")
    if a.overlay:
        (t / "overlay").write_text("1\n")
    for unit in ("talos-vision.service", "mediamtx.service"):
        shutil.copy(NODE / "system" / unit, t / unit)
    shutil.copy(NODE / "system" / "firstrun.sh", boot / "firstrun.sh")
    set_config_txt(boot / "config.txt")
    set_cmdline(boot / "cmdline.txt")
    camera = a.frigate_name or a.device_id
    return {
        "mediamtx": mtx_note,
        "hub_device": {"device_id": a.device_id,
                       "token_sha256": hashlib.sha256(token.encode()).hexdigest()},
        "frigate_camera": {camera: {"deviceId": f"{a.device_id}_video", "token": "<its own token>",
                                    "rtspUrl": f"rtsp://talos:{{FRIGATE_RTSP_PASSWORD}}@{hostname}.local:8554/cam"}},
        "secrets": {"device_token": token, "rtsp_password": password},
    }


def main(argv: list | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--boot", required=True, help="mounted boot partition (macOS: /Volumes/bootfs)")
    ap.add_argument("--device-id", required=True)
    ap.add_argument("--property-id", default="house-01")
    ap.add_argument("--hub-url", required=True)
    ap.add_argument("--mediamtx", required=True, help="mediamtx_<v>_linux_arm64.tar.gz")
    ap.add_argument("--checksums", help="the release's checksums.sha256")
    ap.add_argument("--token", help="device token (default: generated)")
    ap.add_argument("--rtsp-password", help="default: generated")
    ap.add_argument("--hostname")
    ap.add_argument("--frigate-name", help="camera name in Frigate (default: the device id)")
    ap.add_argument("--ntp", help="NTP server when the LAN has no internet (e.g. the router)")
    ap.add_argument("--overlay", action="store_true", help="read-only root file system (VB-03)")
    ap.add_argument("--detect-stream", action="store_true", help="second 640x360 stream (VB-08)")
    a = ap.parse_args(argv)
    try:
        out = provision(a)
    except (ValueError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    print(f"MediaMTX: {out['mediamtx']}")
    d = out["hub_device"]
    print("\nAdd to the hub config's devices (and the camera's _video device for the Frigate connector):")
    print(f"  - {{device_id: {d['device_id']}, token_sha256: {d['token_sha256']}}}")
    print("\nFrigate camera map entry (deploy/frigate/README.md):")
    print("  " + json.dumps(out["frigate_camera"]))
    print("\nSECRETS, shown once. Keep them in your password manager, not in git:")
    print(f"  device token:  {out['secrets']['device_token']}")
    print(f"  RTSP password: {out['secrets']['rtsp_password']}  (FRIGATE_RTSP_PASSWORD)")
    print("\nEject the card, put it in the Zero, plug the PoE cable in. First boot installs and reboots.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
