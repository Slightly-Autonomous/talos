#!/usr/bin/env bash
# Build the production profile and sign the bootloader and app with the Secure Boot v2 key.
#   tools/provision/sign_firmware.sh [key.pem]    (needs ESP-IDF v5.5 exported: . $IDF_PATH/export.sh)
#
# Default key: secrets/secure_boot_signing_key.pem, the local development key (secrets/README.md).
# Burns nothing: flashing and the first boot do that (docs/hardware/provisioning.md).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
key="${1:-$root/secrets/secure_boot_signing_key.pem}"
[ -f "$key" ] || { echo "no signing key at $key" >&2; exit 2; }
command -v idf.py >/dev/null || { echo "export ESP-IDF first (. \$IDF_PATH/export.sh)" >&2; exit 2; }
cd "$root/firmware/esp_idf"
idf.py -B build_production -D SDKCONFIG=build_production/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.production" set-target esp32h2 build
out=build_production/signed
mkdir -p "$out"
espsecure.py sign_data --version 2 --keyfile "$key" \
  --output "$out/bootloader-signed.bin" build_production/bootloader/bootloader.bin
espsecure.py sign_data --version 2 --keyfile "$key" \
  --output "$out/talos_node-signed.bin" build_production/talos_node.bin
espsecure.py verify_signature --version 2 --keyfile "$key" "$out/talos_node-signed.bin"
echo "signed images in firmware/esp_idf/$out"
