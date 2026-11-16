// Bearer-token authentication for the hub API (local mode).
//
// Configs store only SHA-256 hashes of tokens. Principals (people, agents)
// call the user API; device credentials may only post evidence for their own
// device id. Authentication says who; authorization stays in core/policy.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "server/config.h"

namespace talos::server {

std::string sha256_hex(std::string_view data);

struct Identity {
  enum class Kind { kPrincipal, kDevice };
  Kind kind = Kind::kPrincipal;
  std::string id;
};

class Authenticator {
 public:
  // With AUTH_MODE_SUPABASE, `jwt_secret` (read from the configured env var)
  // enables Supabase JWTs alongside local tokens; `clock` checks expiry.
  explicit Authenticator(const talos::server::v1::HubConfig& hub, std::string jwt_secret = "",
                         const IClock* clock = nullptr);
  // `authorization` is the raw Authorization header value.
  std::optional<Identity> authenticate(std::string_view authorization) const;

 private:
  std::map<std::string, Identity> by_hash_;
  std::string jwt_secret_;
  std::string audience_;
  std::map<std::string, std::string> supabase_users_;  // sub -> principal id
  const IClock* clock_;
};

}  // namespace talos::server
