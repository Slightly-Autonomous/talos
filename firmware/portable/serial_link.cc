#include "firmware/portable/serial_link.h"

#include <cstring>

namespace talos::fw {

namespace base64 {

namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
}  // namespace

std::size_t encode(const std::uint8_t* in, std::size_t n, char* out, std::size_t cap) {
  const std::size_t len = (n + 2) / 3 * 4;
  if (len > cap) return 0;
  std::size_t o = 0;
  for (std::size_t i = 0; i < n; i += 3) {
    const std::uint32_t b0 = in[i];
    const std::uint32_t b1 = i + 1 < n ? in[i + 1] : 0;
    const std::uint32_t b2 = i + 2 < n ? in[i + 2] : 0;
    const std::uint32_t v = (b0 << 16) | (b1 << 8) | b2;
    out[o++] = kAlphabet[(v >> 18) & 63];
    out[o++] = kAlphabet[(v >> 12) & 63];
    out[o++] = i + 1 < n ? kAlphabet[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? kAlphabet[v & 63] : '=';
  }
  return o;
}

std::optional<std::size_t> decode(const char* in, std::size_t n, std::uint8_t* out,
                                  std::size_t cap) {
  if (n % 4 != 0) return std::nullopt;
  std::size_t o = 0;
  for (std::size_t i = 0; i < n; i += 4) {
    int v[4];
    for (int k = 0; k < 4; ++k)
      v[k] = in[i + static_cast<std::size_t>(k)] == '='
                 ? -2
                 : value(in[i + static_cast<std::size_t>(k)]);
    if (v[0] < 0 || v[1] < 0 || v[2] == -1 || v[3] == -1) return std::nullopt;
    if ((v[2] == -2 || v[3] == -2) && i + 4 != n) return std::nullopt;  // padding only at the end
    if (v[2] == -2 && v[3] != -2) return std::nullopt;
    const std::uint32_t bits = (static_cast<std::uint32_t>(v[0]) << 18) |
                               (static_cast<std::uint32_t>(v[1]) << 12) |
                               (static_cast<std::uint32_t>(v[2] < 0 ? 0 : v[2]) << 6) |
                               static_cast<std::uint32_t>(v[3] < 0 ? 0 : v[3]);
    const std::size_t bytes = v[2] == -2 ? 1 : v[3] == -2 ? 2 : 3;
    if (o + bytes > cap) return std::nullopt;
    out[o++] = static_cast<std::uint8_t>(bits >> 16);
    if (bytes > 1) out[o++] = static_cast<std::uint8_t>(bits >> 8);
    if (bytes > 2) out[o++] = static_cast<std::uint8_t>(bits);
  }
  return o;
}

}  // namespace base64

// --- SerialDatagram ---------------------------------------------------------------------

bool SerialDatagram::send(const std::uint8_t* data, std::size_t n) {
  if (n > kMaxDatagram) return false;
  char line[kMaxLine + 1];
  std::memcpy(line, kPrefix, 3);
  const std::size_t len = base64::encode(data, n, line + 3, kMaxLine - 4);
  if (len == 0 && n != 0) return false;
  line[3 + len] = '\n';
  out_.write(line, 4 + len);
  return true;
}

bool SerialDatagram::push_line(const char* line, std::size_t n) {
  while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) --n;
  if (n < 3 || std::strncmp(line, kPrefix, 3) != 0) return false;
  if (size_ == kInbox) {
    ++dropped_;  // the runtime's retry covers it
    return true;
  }
  Slot& s = inbox_[(head_ + size_) % kInbox];
  const auto len = base64::decode(line + 3, n - 3, s.data.data(), s.data.size());
  if (!len) {
    ++dropped_;
    return true;
  }
  s.len = *len;
  ++size_;
  return true;
}

std::size_t SerialDatagram::receive(std::uint8_t* buf, std::size_t cap) {
  if (size_ == 0) return 0;
  const Slot& s = inbox_[head_];
  head_ = (head_ + 1) % kInbox;
  --size_;
  if (s.len > cap) {
    ++dropped_;
    return 0;
  }
  std::memcpy(buf, s.data.data(), s.len);
  return s.len;
}

// --- identity ---------------------------------------------------------------------------

std::optional<DeviceIdentity> load_identity(IStore& store) {
  DeviceIdentity id;
  const auto n = store.get(StoreKey::kIdentity, reinterpret_cast<std::uint8_t*>(&id), sizeof id);
  if (!n || *n != sizeof id) return std::nullopt;
  id.device_id[DeviceIdentity::kMaxId] = 0;
  if (!id.valid()) return std::nullopt;
  return id;
}

bool save_identity(IStore& store, const DeviceIdentity& id) {
  if (!id.valid()) return false;
  return store.put(StoreKey::kIdentity, reinterpret_cast<const std::uint8_t*>(&id), sizeof id);
}

ProvisionedLink::ProvisionedLink(IDatagram& net, IStore& store) {
  if (auto id = load_identity(store)) {
    id_ = *id;
    link_.emplace(net, id_.device_id, wire::Key{id_.key, id_.key_len});
  }
}

}  // namespace talos::fw
