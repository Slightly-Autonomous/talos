#include "common/coap/coap.h"

namespace talos::coap {

bool Message::add_segment(std::string_view s) {
  if (path_count >= kMaxPathSegments || s.size() > kMaxSegmentLen) return false;
  auto& seg = path[path_count++];
  for (std::size_t i = 0; i < s.size(); ++i) seg[i] = s[i];
  seg[s.size()] = '\0';
  return true;
}

namespace {

// Reads an extended option nibble (13 -> +1 byte, 14 -> +2 bytes).
bool read_ext(std::uint32_t nibble, const std::uint8_t*& p, const std::uint8_t* end,
              std::uint32_t& out) {
  if (nibble < 13) {
    out = nibble;
  } else if (nibble == 13) {
    if (end - p < 1) return false;
    out = 13u + *p++;
  } else if (nibble == 14) {
    if (end - p < 2) return false;
    out = 269u + static_cast<std::uint32_t>((p[0] << 8) | p[1]);
    p += 2;
  } else {
    return false;  // 15 is reserved (payload marker handled by caller)
  }
  return true;
}

}  // namespace

bool parse(const std::uint8_t* in, std::size_t n, Message& out) {
  out = Message{};
  if (n < 4) return false;
  if ((in[0] >> 6) != 1) return false;  // version 1
  out.type = static_cast<Type>((in[0] >> 4) & 0x3);
  out.token_len = in[0] & 0x0F;
  if (out.token_len > 8) return false;
  out.code = in[1];
  out.message_id = static_cast<std::uint16_t>((in[2] << 8) | in[3]);
  const std::uint8_t* p = in + 4;
  const std::uint8_t* end = in + n;
  if (end - p < out.token_len) return false;
  for (std::size_t i = 0; i < out.token_len; ++i) out.token[i] = *p++;
  std::uint32_t option = 0;
  while (p < end) {
    if (*p == 0xFF) {
      ++p;
      if (p == end) return false;  // marker with empty payload is malformed
      out.payload = p;
      out.payload_len = static_cast<std::size_t>(end - p);
      return true;
    }
    const std::uint8_t b = *p++;
    std::uint32_t delta = 0, len = 0;
    if (!read_ext(b >> 4, p, end, delta) || !read_ext(b & 0x0F, p, end, len)) return false;
    option += delta;
    if (static_cast<std::uint32_t>(end - p) < len) return false;
    if (option == kOptionUriPath) {
      if (!out.add_segment(std::string_view(reinterpret_cast<const char*>(p), len))) return false;
    }
    p += len;
  }
  return true;
}

std::size_t build(const Message& m, std::uint8_t* out, std::size_t cap) {
  std::size_t i = 0;
  auto put = [&](std::uint8_t b) {
    if (i < cap) out[i] = b;
    ++i;
  };
  put(static_cast<std::uint8_t>((1 << 6) | (static_cast<std::uint8_t>(m.type) << 4) | m.token_len));
  put(m.code);
  put(static_cast<std::uint8_t>(m.message_id >> 8));
  put(static_cast<std::uint8_t>(m.message_id & 0xFF));
  for (std::size_t t = 0; t < m.token_len; ++t) put(m.token[t]);
  std::uint32_t last = 0;
  for (std::size_t s = 0; s < m.path_count; ++s) {
    const std::string_view seg = m.segment(s);
    const std::uint32_t delta = kOptionUriPath - last;
    last = kOptionUriPath;
    const auto len = static_cast<std::uint32_t>(seg.size());
    const std::uint8_t dn = static_cast<std::uint8_t>(delta < 13 ? delta : 13);
    const std::uint8_t ln = static_cast<std::uint8_t>(len < 13 ? len : 13);
    put(static_cast<std::uint8_t>((dn << 4) | ln));
    if (delta >= 13) put(static_cast<std::uint8_t>(delta - 13));
    if (len >= 13) put(static_cast<std::uint8_t>(len - 13));
    for (char c : seg) put(static_cast<std::uint8_t>(c));
  }
  if (m.payload_len > 0) {
    put(0xFF);
    for (std::size_t k = 0; k < m.payload_len; ++k) put(m.payload[k]);
  }
  return i <= cap ? i : 0;
}

}  // namespace talos::coap
