// SHA-256 (FIPS 180-4), used only to hash API bearer tokens so configs never
// hold raw secrets. Verified against NIST test vectors in auth_test.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace talos::server {

std::array<std::uint8_t, 32> sha256(std::string_view data);

}  // namespace talos::server
