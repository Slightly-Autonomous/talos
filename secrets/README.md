# Development signing keys (local only)

The owner asked (2026-11-22) for development keys so the first three bench boards can be provisioned
and updated without an offline key ceremony. They were generated on the development machine, in this
folder. **Everything here except this README is git-ignored**: an automated safety check blocked
committing private keys. If the owner wants them in the private repository anyway, they commit them
by hand (`git add -f secrets/<file>`), knowing that a key in git counts as compromised.

| File | What | Used by |
|---|---|---|
| `secure_boot_signing_key.pem` | RSA-3072 Secure Boot v2 signing key (`openssl genrsa 3072`, the same format as `espsecure.py generate_signing_key --scheme rsa3072`) | `tools/provision/sign_firmware.sh` signs the bootloader and app of `sdkconfig.production` builds |
| `firmware_release_ed25519.seed` | Ed25519 seed (hex) for OTA release manifests (`talos_hub keygen`) | `talos_hub sign-release` |
| `firmware_release_ed25519.pub` | Its public key (hex), for the hub's `firmware_signing_keys` | hub config |

**Before any node leaves the bench**, meaning given away, sold, or installed anywhere but the owner's own
bench:
1. Generate new keys offline (`docs/hardware/provisioning.md` steps 1–2).
2. Treat nodes burned with these keys as bench-only. A Secure Boot digest in eFuse is permanent, so such
   a node only ever runs images signed with the key it was burned with.
3. If the keys were ever committed, purge them from history (`git filter-repo --path secrets
   --invert-paths`) and force-push.
4. Close the open-decisions P1 row "Development signing keys".

Never put Wi-Fi or Thread credentials, device keys or customer data here.
