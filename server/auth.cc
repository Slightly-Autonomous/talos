#include "server/auth.h"

#include "server/sha256.h"

namespace talos::server {

std::string sha256_hex(std::string_view data) {
  const auto digest = sha256(data);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (unsigned char b : digest) {
    out += kHex[b >> 4];
    out += kHex[b & 0xF];
  }
  return out;
}

namespace {
bool constant_time_equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return diff == 0;
}
}  // namespace

Authenticator::Authenticator(const talos::server::v1::HubConfig& hub) {
  for (const auto& p : hub.principals()) {
    if (!p.token_sha256().empty())
      by_hash_[p.token_sha256()] = {Identity::Kind::kPrincipal, p.id()};
  }
  for (const auto& d : hub.devices()) {
    by_hash_[d.token_sha256()] = {Identity::Kind::kDevice, d.device_id()};
  }
}

std::optional<Identity> Authenticator::authenticate(std::string_view header) const {
  constexpr std::string_view kBearer = "Bearer ";
  if (header.size() <= kBearer.size() || header.substr(0, kBearer.size()) != kBearer) {
    return std::nullopt;
  }
  const std::string hash = sha256_hex(header.substr(kBearer.size()));
  // Hash-then-compare: the map lookup compares digests, never raw tokens.
  // Confirm with a constant-time comparison of the matched digest.
  auto it = by_hash_.find(hash);
  if (it == by_hash_.end() || !constant_time_equal(it->first, hash)) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace talos::server
