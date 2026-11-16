// Minimal CoAP (RFC 7252) message codec: header, token, options (delta
// encoded, Uri-Path), payload. Portable (no heap): used by the device link
// and the hub ingress. Blockwise, observe and DTLS are out of scope; Thread
// provides link-layer security and payloads carry an HMAC (ADR 0014).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace talos::coap {

enum class Type : std::uint8_t { kCon = 0, kNon = 1, kAck = 2, kRst = 3 };

// Code = class.detail packed as (class << 5) | detail.
inline constexpr std::uint8_t code(std::uint8_t cls, std::uint8_t detail) {
  return static_cast<std::uint8_t>((cls << 5) | detail);
}
inline constexpr std::uint8_t kPost = code(0, 2);
inline constexpr std::uint8_t kChanged = code(2, 4);
inline constexpr std::uint8_t kBadRequest = code(4, 0);
inline constexpr std::uint8_t kUnauthorized = code(4, 1);
inline constexpr std::uint8_t kNotFound = code(4, 4);
inline constexpr std::uint8_t kTooManyRequests = code(4, 29);
inline constexpr std::uint8_t kInternalError = code(5, 0);

inline constexpr std::uint16_t kOptionUriPath = 11;
inline constexpr std::size_t kMaxPathSegments = 4;
inline constexpr std::size_t kMaxSegmentLen = 32;

struct Message {
  Type type = Type::kCon;
  std::uint8_t code = 0;
  std::uint16_t message_id = 0;
  std::uint8_t token_len = 0;
  std::array<std::uint8_t, 8> token{};
  std::size_t path_count = 0;
  std::array<std::array<char, kMaxSegmentLen + 1>, kMaxPathSegments> path{};
  const std::uint8_t* payload = nullptr;  // points into the parsed buffer
  std::size_t payload_len = 0;

  std::string_view segment(std::size_t i) const { return std::string_view(path[i].data()); }
  bool add_segment(std::string_view s);
};

// Returns false for malformed input (never reads past n).
bool parse(const std::uint8_t* in, std::size_t n, Message& out);
// Serializes; returns bytes written or 0 if cap is too small.
std::size_t build(const Message& m, std::uint8_t* out, std::size_t cap);

}  // namespace talos::coap
