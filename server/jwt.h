// HS256 JWT verification for Supabase-issued access tokens (ADR 0015).
// Rejects any algorithm other than HS256 (including "none"), bad signatures,
// expired or not-yet-valid tokens, and audience mismatches.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace talos::server {

struct JwtClaims {
  std::string subject;
  std::int64_t expires_at_s = 0;
};

std::optional<JwtClaims> verify_hs256(std::string_view token, std::string_view secret,
                                      std::string_view audience, std::int64_t now_s);

// Exposed for tests and tooling.
std::string base64url_encode(std::string_view data);
std::optional<std::string> base64url_decode(std::string_view in);

}  // namespace talos::server
