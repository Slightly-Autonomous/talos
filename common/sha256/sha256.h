// SHA-256 (FIPS 180-4). Used to hash API bearer tokens (configs never hold
// raw secrets) and to verify OTA images on devices. Streaming, no heap, no
// exceptions. NIST vectors in //server:server_test and the firmware tests.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace talos {

class Sha256 {
 public:
  Sha256();
  void update(const std::uint8_t* data, std::size_t n);
  void update(std::string_view s) {
    update(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
  }
  std::array<std::uint8_t, 32> finish();

 private:
  void block(const std::uint8_t* p);
  std::uint32_t h_[8];
  std::uint8_t buf_[64];
  std::size_t buf_len_ = 0;
  std::uint64_t total_ = 0;
};

std::array<std::uint8_t, 32> sha256(std::string_view data);

}  // namespace talos
