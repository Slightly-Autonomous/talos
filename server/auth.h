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
  explicit Authenticator(const talos::server::v1::HubConfig& hub);
  // `authorization` is the raw Authorization header value.
  std::optional<Identity> authenticate(std::string_view authorization) const;

 private:
  std::map<std::string, Identity> by_hash_;
};

}  // namespace talos::server
