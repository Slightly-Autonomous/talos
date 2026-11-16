// Firmware release signatures (ADR 0014): Ed25519 via libsodium over a
// canonical message binding board, version, size and image digest. The hub
// only offers releases whose signature verifies against a configured owner
// key and whose image matches the signed digest. Owners hold their own
// signing keys (spec §36): no vendor key is baked in.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/base/result.h"
#include "talos/server/v1/config.pb.h"
#include "talos/v1/firmware.pb.h"

namespace talos::server {

std::string canonical_release_message(const talos::v1::FirmwareRelease& m);

Status verify_release(
    const talos::v1::FirmwareRelease& manifest, const std::string& image,
    const google::protobuf::RepeatedPtrField<talos::server::v1::SigningKey>& keys);

struct KeyPair {
  std::string seed_hex;        // secret: keep offline
  std::string public_key_hex;  // goes into firmware_signing_keys
};
Result<KeyPair> generate_keypair();
// Fills image_sha256, image_size, signature and signing_key_id.
Status sign_release(talos::v1::FirmwareRelease& manifest, const std::string& image,
                    const std::string& seed_hex, const std::string& key_id);

std::string to_hex(const std::uint8_t* p, std::size_t n);
bool from_hex(const std::string& hex, std::vector<std::uint8_t>& out);

}  // namespace talos::server
