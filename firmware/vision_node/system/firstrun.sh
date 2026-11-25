#!/bin/bash
# TL-VN-001 first boot (ADR 0020). Runs once as root from the boot partition, the way Raspberry Pi
# Imager's firstrun.sh does: cmdline.txt carries systemd.run=/boot/firmware/firstrun.sh, and this
# script removes that again. Everything it installs was put on the boot partition by
# tools/provision/vision_node.py. Only python3-libgpiod may need the network.
set -euo pipefail
B=/boot/firmware
T=$B/talos
exec >>"$B/talos-firstrun.log" 2>&1
echo "firstrun $(date -u)"

id talos >/dev/null 2>&1 || useradd --system --create-home --home-dir /var/lib/talos --shell /usr/sbin/nologin talos
usermod -aG gpio,video talos

install -d -m 0755 /opt/talos/vision /etc/talos
cp -r "$T/agent/talos_vision" /opt/talos/vision/
install -m 0755 "$T/mediamtx" /usr/local/bin/mediamtx
install -m 0640 -o root -g talos "$T/mediamtx.yml" /etc/talos/mediamtx.yml
install -m 0644 "$T/vision.json" /etc/talos/vision.json
install -m 0640 -o root -g talos "$T/device.token" /etc/talos/device.token
install -m 0644 "$T/talos-vision.service" "$T/mediamtx.service" /etc/systemd/system/
# Secrets leave the FAT partition once installed.
shred -u "$T/device.token" "$T/mediamtx.yml" || rm -f "$T/device.token" "$T/mediamtx.yml"

hostnamectl set-hostname "$(cat "$T/hostname")"
if [ -s "$T/ntp" ]; then
  mkdir -p /etc/systemd/timesyncd.conf.d
  printf '[Time]\nNTP=%s\n' "$(cat "$T/ntp")" > /etc/systemd/timesyncd.conf.d/talos.conf
fi
# Hardware watchdog: systemd pets it; a hung system resets within 15 s.
mkdir -p /etc/systemd/system.conf.d
printf '[Manager]\nRuntimeWatchdogSec=15\n' > /etc/systemd/system.conf.d/talos-watchdog.conf

# The lid switch needs libgpiod's Python binding; Lite images may not ship it.
if ! dpkg -s python3-libgpiod >/dev/null 2>&1; then
  apt-get install -y --no-install-recommends python3-libgpiod ||
    echo "WARNING: python3-libgpiod missing and no network: the agent cannot read the lid"
fi

systemctl enable systemd-time-wait-sync.service mediamtx.service talos-vision.service
systemctl disable --now bluetooth.service hciuart.service 2>/dev/null || true

# Read-only root (overlay) so power cuts can't damage the file system (VB-03).
if [ -e "$T/overlay" ]; then raspi-config nonint do_overlayfs 0 || echo "overlay setup failed"; fi

sed -i 's| systemd.run=[^ ]*||; s| systemd.run_success_action=[^ ]*||; s| systemd.unit=kernel-command-line.target||' "$B/cmdline.txt"
rm -f "$B/firstrun.sh"
echo "firstrun done"
exit 0
