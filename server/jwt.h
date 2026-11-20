// HS256 JWT verification for Supabase-issued access tokens (ADR 0015).
// Rejects any algorithm other than HS256 (including "none"), bad signatures,
// expired or not-yet-valid tokens, out-of-range time claims, and audience
// (and, when configured, issuer) mismatches.
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

// `issuer`, when non-empty, must equal the "iss" claim.
std::optional<JwtClaims> verify_hs256(std::string_view token, std::string_view secret,
                                      std::string_view audience, std::int64_t now_s,
                                      std::string_view issuer = {});

// Exposed for tests and tooling.
std::string base64url_encode(std::string_view data);
std::optional<std::string> base64url_decode(std::string_view in);

}  // namespace talos::server
