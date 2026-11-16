// Device <-> hub wire format v1 (ADR 0014): what CoAP payloads carry.
// Fixed little-endian layouts, explicit lengths, and an 8-byte truncated
// HMAC-SHA256 tag keyed by the device's provisioned secret. The downlink tag
// also covers the request tag, binding each reply to the request it answers
// (no replay of old downlinks). Shared by firmware and hub: one codec.
//
//   uplink:   u8 version=1 | u8 count | count x Event(28 B) | tag(8)
//   downlink: u8 version=1 | u8 count | items...            | tag(8)
//     item:   u8 type | body (Ack 8 B, Config 16 B, OtaOffer 40 B,
//                             OtaChunk 6 B + data, Reboot 0 B)
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "firmware/portable/types.h"

namespace talos::fw::wire {

inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kTagLen = 8;
inline constexpr std::size_t kEventLen = 28;
inline constexpr std::size_t kMaxFrame = 1024;
inline constexpr std::size_t kMaxUplinkEvents = 16;

using Tag = std::array<std::uint8_t, kTagLen>;

struct Key {
  const std::uint8_t* data = nullptr;
  std::size_t len = 0;
};

enum class DecodeError : std::uint8_t {
  kNone,
  kTooShort,
  kBadVersion,
  kBadLength,
  kBadTag,
  kBadItem
};

// Encodes events into out; returns bytes written (0 on overflow).
std::size_t encode_uplink(const DeviceEvent* events, std::size_t count, Key key, std::uint8_t* out,
                          std::size_t cap);

// Calls on_event for each event after verifying the tag over the whole frame.
template <typename F>
DecodeError decode_uplink(const std::uint8_t* in, std::size_t n, Key key, Tag* tag_out,
                          F&& on_event);

// Downlink builder: add items, then finish() with the request's tag.
class DownlinkWriter {
 public:
  DownlinkWriter(std::uint8_t* out, std::size_t cap) : out_(out), cap_(cap) {}
  bool add(const Downlink& d);  // false if it does not fit
  std::size_t finish(Key key, const Tag& request_tag);
  std::size_t remaining() const { return cap_ > len_ + kTagLen ? cap_ - len_ - kTagLen : 0; }

 private:
  std::uint8_t* out_;
  std::size_t cap_;
  std::size_t len_ = 2;
  std::uint8_t count_ = 0;
};

template <typename F>
DecodeError decode_downlink(const std::uint8_t* in, std::size_t n, Key key, const Tag& request_tag,
                            F&& on_item);

// --- implementation details used by the templates ------------------------------------

Tag compute_tag(Key key, const std::uint8_t* data, std::size_t n, const Tag* prefix);
bool decode_event(const std::uint8_t* p, DeviceEvent& e);
// Parses one item at p (bounded by end); returns bytes consumed, 0 on error.
std::size_t decode_item(const std::uint8_t* p, const std::uint8_t* end, Downlink& d);
bool tag_equal(const Tag& a, const std::uint8_t* b);

template <typename F>
DecodeError decode_uplink(const std::uint8_t* in, std::size_t n, Key key, Tag* tag_out,
                          F&& on_event) {
  if (n < 2 + kTagLen) return DecodeError::kTooShort;
  if (in[0] != kVersion) return DecodeError::kBadVersion;
  const std::size_t count = in[1];
  if (count == 0 || count > kMaxUplinkEvents || n != 2 + count * kEventLen + kTagLen) {
    return DecodeError::kBadLength;
  }
  const Tag tag = compute_tag(key, in, n - kTagLen, nullptr);
  if (!tag_equal(tag, in + n - kTagLen)) return DecodeError::kBadTag;
  if (tag_out) *tag_out = tag;
  for (std::size_t i = 0; i < count; ++i) {
    DeviceEvent e;
    if (!decode_event(in + 2 + i * kEventLen, e)) return DecodeError::kBadItem;
    on_event(e);
  }
  return DecodeError::kNone;
}

template <typename F>
DecodeError decode_downlink(const std::uint8_t* in, std::size_t n, Key key, const Tag& request_tag,
                            F&& on_item) {
  if (n < 2 + kTagLen) return DecodeError::kTooShort;
  if (in[0] != kVersion) return DecodeError::kBadVersion;
  const Tag tag = compute_tag(key, in, n - kTagLen, &request_tag);
  if (!tag_equal(tag, in + n - kTagLen)) return DecodeError::kBadTag;
  const std::uint8_t* p = in + 2;
  const std::uint8_t* end = in + n - kTagLen;
  for (std::size_t i = 0; i < in[1]; ++i) {
    Downlink d;
    const std::size_t used = decode_item(p, end, d);
    if (used == 0) return DecodeError::kBadItem;
    p += used;
    on_item(d);
  }
  return p == end ? DecodeError::kNone : DecodeError::kBadLength;
}

}  // namespace talos::fw::wire
