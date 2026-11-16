#include "server/release_signing.h"

#include <sodium.h>

#include "common/sha256/sha256.h"

namespace talos::server {

std::string to_hex(const std::uint8_t* p, std::size_t n) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < n; ++i) {
    out += kHex[p[i] >> 4];
    out += kHex[p[i] & 0xF];
  }
  return out;
}

bool from_hex(const std::string& hex, std::vector<std::uint8_t>& out) {
  if (hex.size() % 2) return false;
  out.clear();
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
  }
  return true;
}

std::string canonical_release_message(const talos::v1::FirmwareRelease& m) {
  return "talos-firmware-release-v1\n" + m.board_id() + "\n" + m.board_revision() + "\n" +
         m.version() + "\n" + std::to_string(m.image_size_bytes()) + "\n" +
         to_hex(reinterpret_cast<const std::uint8_t*>(m.image_sha256().data()),
                m.image_sha256().size()) +
         "\n";
}

Status verify_release(
    const talos::v1::FirmwareRelease& m, const std::string& image,
    const google::protobuf::RepeatedPtrField<talos::server::v1::SigningKey>& keys) {
  if (sodium_init() < 0) return Error{"crypto_unavailable", "libsodium init failed"};
  if (image.size() != m.image_size_bytes()) return Error{"invalid_release", "image size mismatch"};
  const auto digest = talos::sha256(image);
  if (m.image_sha256().size() != digest.size() ||
      !talos::constant_time_equal(digest.data(),
                                  reinterpret_cast<const std::uint8_t*>(m.image_sha256().data()),
                                  digest.size())) {
    return Error{"invalid_release", "image digest does not match the manifest"};
  }
  if (m.signature().size() != crypto_sign_BYTES)
    return Error{"invalid_release", "missing signature"};
  const std::string msg = canonical_release_message(m);
  for (const auto& k : keys) {
    if (k.key_id() != m.signing_key_id()) continue;
    std::vector<std::uint8_t> pk;
    if (!from_hex(k.public_key_hex(), pk) || pk.size() != crypto_sign_PUBLICKEYBYTES) continue;
    if (crypto_sign_verify_detached(reinterpret_cast<const unsigned char*>(m.signature().data()),
                                    reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
                                    pk.data()) == 0) {
      return Status::Ok();
    }
    return Error{"invalid_release", "signature does not verify for key " + k.key_id()};
  }
  return Error{"invalid_release", "unknown signing key '" + m.signing_key_id() + "'"};
}

Result<KeyPair> generate_keypair() {
  if (sodium_init() < 0) return Error{"crypto_unavailable", "libsodium init failed"};
  unsigned char seed[crypto_sign_SEEDBYTES];
  unsigned char pk[crypto_sign_PUBLICKEYBYTES];
  unsigned char sk[crypto_sign_SECRETKEYBYTES];
  randombytes_buf(seed, sizeof seed);
  crypto_sign_seed_keypair(pk, sk, seed);
  KeyPair kp{to_hex(seed, sizeof seed), to_hex(pk, sizeof pk)};
  sodium_memzero(seed, sizeof seed);
  sodium_memzero(sk, sizeof sk);
  return kp;
}

Status sign_release(talos::v1::FirmwareRelease& m, const std::string& image,
                    const std::string& seed_hex, const std::string& key_id) {
  if (sodium_init() < 0) return Error{"crypto_unavailable", "libsodium init failed"};
  std::vector<std::uint8_t> seed;
  if (!from_hex(seed_hex, seed) || seed.size() != crypto_sign_SEEDBYTES) {
    return Error{"invalid_key", "seed must be 32 bytes of hex"};
  }
  unsigned char pk[crypto_sign_PUBLICKEYBYTES];
  unsigned char sk[crypto_sign_SECRETKEYBYTES];
  crypto_sign_seed_keypair(pk, sk, seed.data());
  const auto digest = talos::sha256(image);
  m.set_image_sha256(std::string(reinterpret_cast<const char*>(digest.data()), digest.size()));
  m.set_image_size_bytes(image.size());
  m.set_signing_key_id(key_id);
  const std::string msg = canonical_release_message(m);
  unsigned char sig[crypto_sign_BYTES];
  crypto_sign_detached(sig, nullptr, reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
                       sk);
  sodium_memzero(sk, sizeof sk);
  m.set_signature(std::string(reinterpret_cast<const char*>(sig), sizeof sig));
  return Status::Ok();
}

}  // namespace talos::server
