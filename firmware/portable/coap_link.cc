#include "firmware/portable/coap_link.h"

#include <cstring>

#include "common/coap/coap.h"

namespace talos::fw {

CoapLink::CoapLink(IDatagram& net, const char* device_id, wire::Key key)
    : net_(net), device_id_(device_id), key_(key) {}

bool CoapLink::send(const DeviceEvent& e) {
  std::uint8_t frame[2 + wire::kEventLen + wire::kTagLen];
  const std::size_t n = wire::encode_uplink(&e, 1, key_, frame, sizeof frame);
  if (n == 0) return false;

  coap::Message m;
  m.type = coap::Type::kCon;
  m.code = coap::kPost;
  m.message_id = next_mid_++;
  m.token_len = 4;
  const std::uint32_t tok = next_token_++;
  for (int i = 0; i < 4; ++i)
    m.token[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(tok >> (8 * i));
  if (!m.add_segment("d") || !m.add_segment(device_id_) || !m.add_segment("e")) return false;
  m.payload = frame;
  m.payload_len = n;
  std::uint8_t out[128];
  const std::size_t len = coap::build(m, out, sizeof out);
  if (len == 0) return false;

  PendingRequest& p = pending_[pending_next_];
  pending_next_ = (pending_next_ + 1) % kPending;
  p.used = true;
  for (std::size_t i = 0; i < 4; ++i) p.token[i] = m.token[i];
  for (std::size_t i = 0; i < wire::kTagLen; ++i) p.tag[i] = frame[n - wire::kTagLen + i];
  return net_.send(out, len);
}

void CoapLink::poll_network() {
  std::uint8_t buf[wire::kMaxFrame + 64];
  for (;;) {
    const std::size_t n = net_.receive(buf, sizeof buf);
    if (n == 0) return;
    coap::Message m;
    if (!coap::parse(buf, n, m) || m.type != coap::Type::kAck || m.code != coap::kChanged ||
        m.token_len != 4 || m.payload_len == 0) {
      ++rejected_;
      continue;
    }
    PendingRequest* req = nullptr;
    for (auto& p : pending_) {
      if (p.used && std::memcmp(p.token.data(), m.token.data(), 4) == 0) req = &p;
    }
    if (!req) {
      ++rejected_;  // unsolicited or late reply
      continue;
    }
    const auto err =
        wire::decode_downlink(m.payload, m.payload_len, key_, req->tag, [&](const Downlink& d) {
          if (inbox_size_ < kInbox) {
            inbox_[(inbox_head_ + inbox_size_) % kInbox] = d;
            ++inbox_size_;
          }
        });
    req->used = false;
    if (err != wire::DecodeError::kNone) ++rejected_;
  }
}

std::optional<Downlink> CoapLink::receive() {
  if (inbox_size_ == 0) poll_network();
  if (inbox_size_ == 0) return std::nullopt;
  Downlink d = inbox_[inbox_head_];
  inbox_head_ = (inbox_head_ + 1) % kInbox;
  --inbox_size_;
  return d;
}

}  // namespace talos::fw
