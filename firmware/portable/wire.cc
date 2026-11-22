#include "firmware/portable/wire.h"

#include "common/sha256/sha256.h"

namespace talos::fw::wire {

namespace {

void put_u16(std::uint8_t*& p, std::uint16_t v) {
  *p++ = static_cast<std::uint8_t>(v);
  *p++ = static_cast<std::uint8_t>(v >> 8);
}
void put_u32(std::uint8_t*& p, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) *p++ = static_cast<std::uint8_t>(v >> (8 * i));
}
void put_u64(std::uint8_t*& p, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) *p++ = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint16_t get_u16(const std::uint8_t*& p) {
  const auto v = static_cast<std::uint16_t>(p[0] | (p[1] << 8));
  p += 2;
  return v;
}
std::uint32_t get_u32(const std::uint8_t*& p) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
  p += 4;
  return v;
}
std::uint64_t get_u64(const std::uint8_t*& p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  p += 8;
  return v;
}

// kConfig2 (2026-11-22) = kConfig + u8 power_profile. Sent only when the profile
// changes, so a hub talking to older firmware keeps sending kConfig.
enum ItemType : std::uint8_t {
  kAck = 1,
  kConfig = 2,
  kOffer = 3,
  kChunk = 4,
  kReboot = 5,
  kConfig2 = 6
};

}  // namespace

Tag compute_tag(Key key, const std::uint8_t* data, std::size_t n, const Tag* prefix) {
  HmacSha256 h(key.data, key.len);
  if (prefix) h.update(prefix->data(), prefix->size());
  h.update(data, n);
  const auto full = h.finish();
  Tag t{};
  for (std::size_t i = 0; i < kTagLen; ++i) t[i] = full[i];
  return t;
}

bool tag_equal(const Tag& a, const std::uint8_t* b) {
  return talos::constant_time_equal(a.data(), b, kTagLen);
}

std::size_t encode_uplink(const DeviceEvent* events, std::size_t count, Key key, std::uint8_t* out,
                          std::size_t cap) {
  const std::size_t n = 2 + count * kEventLen + kTagLen;
  if (count == 0 || count > kMaxUplinkEvents || n > cap) return 0;
  std::uint8_t* p = out;
  *p++ = kVersion;
  *p++ = static_cast<std::uint8_t>(count);
  for (std::size_t i = 0; i < count; ++i) {
    const DeviceEvent& e = events[i];
    put_u32(p, e.boot_epoch);
    put_u32(p, e.seq);
    *p++ = e.slot;
    *p++ = static_cast<std::uint8_t>(e.kind);
    *p++ = static_cast<std::uint8_t>(e.priority);
    *p++ = e.flag ? 1 : 0;
    put_u64(p, e.at_ms);
    put_u32(p, static_cast<std::uint32_t>(e.a));
    put_u32(p, e.b);
  }
  const Tag tag = compute_tag(key, out, static_cast<std::size_t>(p - out), nullptr);
  for (auto b : tag) *p++ = b;
  return n;
}

bool decode_event(const std::uint8_t* p, DeviceEvent& e) {
  e.boot_epoch = get_u32(p);
  e.seq = get_u32(p);
  e.slot = *p++;
  const std::uint8_t kind = *p++;
  const std::uint8_t prio = *p++;
  const std::uint8_t flags = *p++;
  if (kind > static_cast<std::uint8_t>(EventKind::kReset) || prio > 3 || flags > 1) return false;
  if (e.slot != kNoSlot && e.slot >= kSlots) return false;
  e.kind = static_cast<EventKind>(kind);
  e.priority = static_cast<Priority>(prio);
  e.flag = flags == 1;
  e.at_ms = get_u64(p);
  e.a = static_cast<std::int32_t>(get_u32(p));
  e.b = get_u32(p);
  return e.seq != 0;
}

bool DownlinkWriter::add(const Downlink& d) {
  std::uint8_t buf[2 + kOtaChunkMax + 8];
  std::uint8_t* p = buf;
  if (const auto* a = std::get_if<Ack>(&d)) {
    *p++ = kAck;
    put_u32(p, a->boot_epoch);
    put_u32(p, a->seq);
  } else if (const auto* c = std::get_if<ConfigUpdate>(&d)) {
    const bool v2 = c->power_profile != ConfigUpdate::kKeepProfile;
    *p++ = v2 ? kConfig2 : kConfig;
    put_u32(p, c->revision);
    put_u32(p, c->heartbeat_interval_ms);
    put_u32(p, c->environment_interval_ms);
    put_u32(p, c->impact_threshold_mg);
    if (v2) *p++ = c->power_profile;
  } else if (const auto* o = std::get_if<OtaOffer>(&d)) {
    *p++ = kOffer;
    put_u32(p, o->version);
    put_u32(p, o->size);
    for (auto b : o->sha256) *p++ = b;
  } else if (const auto* ch = std::get_if<OtaChunk>(&d)) {
    if (ch->length > kOtaChunkMax) return false;
    *p++ = kChunk;
    put_u32(p, ch->offset);
    put_u16(p, ch->length);
    for (std::size_t i = 0; i < ch->length; ++i) *p++ = ch->data[i];
  } else {
    *p++ = kReboot;
  }
  const auto n = static_cast<std::size_t>(p - buf);
  if (count_ == 255 || len_ + n + kTagLen > cap_) return false;
  for (std::size_t i = 0; i < n; ++i) out_[len_ + i] = buf[i];
  len_ += n;
  ++count_;
  return true;
}

std::size_t DownlinkWriter::finish(Key key, const Tag& request_tag) {
  out_[0] = kVersion;
  out_[1] = count_;
  const Tag tag = compute_tag(key, out_, len_, &request_tag);
  for (std::size_t i = 0; i < kTagLen; ++i) out_[len_ + i] = tag[i];
  return len_ + kTagLen;
}

std::size_t decode_item(const std::uint8_t* p, const std::uint8_t* end, Downlink& d) {
  const std::uint8_t* start = p;
  auto have = [&](std::size_t n) { return static_cast<std::size_t>(end - p) >= n; };
  if (!have(1)) return 0;
  switch (*p++) {
    case kAck: {
      if (!have(8)) return 0;
      Ack a;
      a.boot_epoch = get_u32(p);
      a.seq = get_u32(p);
      d = a;
      break;
    }
    case kConfig:
    case kConfig2: {
      const bool v2 = p[-1] == kConfig2;
      if (!have(v2 ? 17 : 16)) return 0;
      ConfigUpdate c;
      c.revision = get_u32(p);
      c.heartbeat_interval_ms = get_u32(p);
      c.environment_interval_ms = get_u32(p);
      c.impact_threshold_mg = get_u32(p);
      if (v2) c.power_profile = *p++;
      d = c;
      break;
    }
    case kOffer: {
      if (!have(40)) return 0;
      OtaOffer o;
      o.version = get_u32(p);
      o.size = get_u32(p);
      for (auto& b : o.sha256) b = *p++;
      d = o;
      break;
    }
    case kChunk: {
      if (!have(6)) return 0;
      OtaChunk c;
      c.offset = get_u32(p);
      c.length = get_u16(p);
      if (c.length == 0 || c.length > kOtaChunkMax || !have(c.length)) return 0;
      for (std::size_t i = 0; i < c.length; ++i) c.data[i] = *p++;
      d = c;
      break;
    }
    case kReboot:
      d = RebootRequest{};
      break;
    default:
      return 0;
  }
  return static_cast<std::size_t>(p - start);
}

}  // namespace talos::fw::wire
