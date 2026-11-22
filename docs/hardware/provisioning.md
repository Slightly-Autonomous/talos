# TL-SN-002 production provisioning (HD-016 resolution path)

**Status:** proposed path, nothing here has run on a board. Owner decision 2026-11-22: the
TL-SN-002 USB-C port **stays reachable from outside the closed enclosure**, so the node is locked
down in firmware and configuration instead of moving the port (ADR 0019 amendment 2026-11-22,
ADR 0013 amendment 2026-11-22, `docs/decisions/open-decisions.md` "TL-SN-002 USB-C attack surface").
This page is how a production node gets there. It resolves HD-016 (secure provisioning) for the
chip once the owner runs it on a real board; until then HD-016 stays open.

**Every step that burns eFuses is irreversible and is the owner's explicit action.** No tool,
script or CI job in this repository burns eFuses, and none may be added that does so on its own.
CI only compiles the production profile, unsigned, with no key.

## What protects the outside port

| Layer | What it stops | Where |
|---|---|---|
| Console read-only while the lid is closed | `provision`, `board-id`, `i2c wr`/`rd`, `rail`, `uart`, `mux`, `rescan`, `reboot`, `led service`, sensor self-tests: refused with `locked: open the enclosure`, counted, reported to the hub (`console.locked`, at most one report a minute). Opening the lid is the P0 tamper alarm | `firmware/portable/console_policy.h`, `BoardSupport::console_lock` |
| Lamps lid-gated | a power bank on the port lights nothing: STAT, LINK, MAG, VIB, PIR, ENV, TAMP, BATT, FLT dark unless the lid is open or service mode (which itself needs the lid open). The USER lamp test runs only with the lid open | `BoardSupport::lamp_gate` |
| Secure Boot v2 (RSA-3072) | loading any image not signed with the owner's key (USB download or OTA) | `sdkconfig.production` |
| Flash encryption, release mode | reading the app or bootloader out of flash, writing chosen plaintext | `sdkconfig.production` |
| JTAG off (`DIS_USB_JTAG`, `DIS_PAD_JTAG`, `SOFT_DIS_JTAG`) | halting the CPU or reading RAM over the USB Serial/JTAG | burned by the bootloader on first boot |
| Secure ROM download mode (`ENABLE_SECURITY_DOWNLOAD`) | flash and memory reads, the esptool stub, `espefuse` writes, manual encryption over USB | burned on first boot |
| Hub health | repeated refusals: device DEGRADED, "USB console access attempts while closed" (never hidden by HEALTHY) | `server/device_registry.cc` |

Residual, accepted (open-decisions.md):
- The hardware **PWR lamp** on VBUS lights whenever USB power is present. It says only that; it
  is not firmware's and stays.
- A powered USB port can power the node. No harm: it changes nothing and lights nothing else.
- The USB host can **reset** the chip (USB Serial/JTAG line-state reset) and can request ROM
  download mode, which is secure-only. A reset is a new boot epoch the hub sees, and a 30-60 s PIR
  warm-up. With download mode the host can erase or overwrite flash: Secure Boot refuses to run
  what it wrote, so the node goes silent and the hub shows it OFFLINE. Optional: burn
  `DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE` (step 7) so USB can no longer enter download mode at all
  (recovery then only over the Tag-Connect UART0 pads, inside the enclosure).
- Without Thread credentials the node's hub link (CoAP frames, `@F` lines) runs over this USB
  console (bench). With `thread <dataset> <hub-ipv6>` provisioned (diag, lid open), the same CoAP
  link runs over OpenThread UDP (`firmware/esp_idf/main/thread_link.cc`); production uses Thread.
- NVS is encrypted in the production profile: both partitions (`nvs`, `talos_id`) use keys
  derived from an HMAC key in eFuse KEY4, generated and burned by the firmware at the first boot.
- Anti-rollback: the bootloader refuses an image below the eFuse secure version
  (`CONFIG_BOOTLOADER_APP_SECURE_VERSION`, 16 bumps available). Bump it only for releases that
  must never be downgraded from.
- A reset through the USB port is reported (`device.reset`, cause USB); with the lid shut it counts
  toward the hub's USB-probing DEGRADED rule.

## Steps (owner, once per node, offline machine for the key)

ESP-IDF v5.5 (the CI image is `espressif/idf:v5.5.6`). Paths are from `firmware/esp_idf`.

1. **Make the signing key, offline, once for the fleet.** Keep it off the repository and off CI
   (`.gitignore` ignores `*.pem` and `secrets/`). For the three bench boards a development key
   already exists in `secrets/` (2026-11-22, `secrets/README.md`); `tools/provision/sign_firmware.sh`
   builds and signs with it. Rotate before any node leaves the bench. Losing it means no more updates for every provisioned node;
   leaking it means anyone can sign firmware for them.
   ```bash
   espsecure.py generate_signing_key --version 2 --scheme rsa3072 secure_boot_signing_key.pem
   ```
   Optionally generate up to three keys (the H2 has three digest slots) and keep the spares apart,
   for revocation.

2. **Build the production profile** (the same command CI compiles):
   ```bash
   idf.py -B build_production -D SDKCONFIG=build_production/sdkconfig \
          -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.production" set-target esp32h2 build
   ```
   It produces **unsigned** images (`CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES` off).

3. **Sign** the bootloader and the app on the offline machine:
   ```bash
   espsecure.py sign_data --version 2 --keyfile secure_boot_signing_key.pem \
       --output build_production/bootloader/bootloader-signed.bin build_production/bootloader/bootloader.bin
   espsecure.py sign_data --version 2 --keyfile secure_boot_signing_key.pem \
       --output build_production/talos_node-signed.bin build_production/talos_node.bin
   ```

4. **Decide on flash-encryption key custody** before the first boot:
   - default: the bootloader generates the key on the chip and read-protects it. Nobody, owner
     included, can reflash over USB afterwards; updates are OTA only (signed). Simplest, strongest.
   - or: generate it offline (`espsecure.py generate_flash_encryption_key flash_enc_key.bin`) and
     burn it before the first boot (`espefuse.py --chip esp32h2 burn_key BLOCK_KEY1 flash_enc_key.bin
     XTS_AES_128_KEY`). The owner can then reflash a bricked node with images pre-encrypted by
     `espsecure.py encrypt_flash_data`. That key then needs the same care as the signing key.

5. **Flash the plaintext, signed images** over USB (download mode: hold BOOT, press RST; the lid is
   open on the bench anyway):
   ```bash
   esptool.py --chip esp32h2 -p PORT --before default_reset --after no_reset write_flash \
       0x0 build_production/bootloader/bootloader-signed.bin \
       0xF000 build_production/partition_table/partition-table.bin \
       0x30000 build_production/talos_node-signed.bin
   ```
   (Offsets from `partitions_production.csv`; check them against `build_production/flash_args`.)

6. **First boot burns the eFuses. IRREVERSIBLE.** On reset the bootloader verifies its own
   signature, burns the public-key digest and `SECURE_BOOT_EN`, generates (or uses) the flash
   encryption key, encrypts the flash in place, burns the JTAG disables, `DIS_DOWNLOAD_MANUAL_ENCRYPT`,
   `DIS_DIRECT_BOOT` and secure download mode, then reboots into the app. Do not remove power during
   it. Check afterwards: `espefuse.py --chip esp32h2 -p PORT summary` (secure download mode still
   allows the summary) shows `SECURE_BOOT_EN`, `DIS_USB_JTAG`, `ENABLE_SECURITY_DOWNLOAD`, the
   flash encryption count, and `DIS_USB_SERIAL_JTAG` **not** burned (the console needs it).

7. **Optional (owner decision):** `espefuse.py --chip esp32h2 -p PORT burn_efuse
   DIS_USB_SERIAL_JTAG_DOWNLOAD_MODE` closes USB download mode for good (see residuals).

8. **Identity and bench** with the lid **open** (the console is read-only while it is closed;
   opening it raises the tamper alarm, so start maintenance on the hub first):
   `python3 -m talos_bench.flash --no-flash --port PORT --unit-serial SN2-A-0007`. It opens with
   a `lid.open` step, writes the board identity, then the plan checks the lock with the lid closed
   (S-M17, S-M20) and opens it again (S-M21). Provision the device key the same way (`provision`
   with the lid open), close the lid, end maintenance.

9. **Updates:** sign every OTA image with the same key (`espsecure.py sign_data --version 2`)
   before adding it to the hub's firmware store; a production node refuses unsigned images.

## Open (tracked under HD-016)
- Bench: NVS encryption's first-boot HMAC key burn and the secure init of `talos_id`;
  anti-rollback across a real OTA; the USB-reset report.
- Everything above is compile-checked only: the eFuse flow, the signed boot, release-mode flash
  encryption and the console over a provisioned chip need a bench run (bench-test-plan §10).
