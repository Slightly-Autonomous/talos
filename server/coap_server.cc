#include "server/coap_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "codec/yaml_loader.h"
#include "common/coap/coap.h"
#include "firmware/portable/wire.h"
#include "server/release_signing.h"

namespace talos::server {

namespace {
constexpr std::size_t kCacheSize = 256;
constexpr std::size_t kReplyCap = 1152;
// Leave room for the CoAP header, token and payload marker.
constexpr std::size_t kDownlinkBudget = fw::wire::kMaxFrame - 2 - fw::wire::kTagLen;
}  // namespace

Result<DeviceKeyMap> load_device_keys(const std::string& path, const Property& property) {
  talos::server::v1::DeviceKeys file;
  if (Status s = codec::load_yaml_file(path, file); !s) return s.error();
  DeviceKeyMap out;
  for (const auto& k : file.keys()) {
    if (!property.find_device(DeviceId(k.device_id()))) {
      return Error{"invalid_config", "device key for unknown device " + k.device_id()};
    }
    std::vector<std::uint8_t> key;
    if (!from_hex(k.key_hex(), key) || key.size() < 16) {
      return Error{"invalid_config",
                   "device " + k.device_id() + ": key must be >= 16 bytes of hex"};
    }
    out[k.device_id()] = std::move(key);
  }
  return out;
}

CoapServer::CoapServer(HubService& hub, DeviceKeyMap keys, std::uint32_t rate, std::uint32_t burst)
    : hub_(hub),
      keys_(std::move(keys)),
      rate_(rate > 0 ? rate : 20),
      burst_(burst > 0 ? burst : 40) {}

CoapServer::~CoapServer() { stop(); }

Result<int> CoapServer::bind(const std::string& host, int port) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) return Error{"bind_failed", "socket"};
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
      ::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    ::close(fd_);
    fd_ = -1;
    return Error{"bind_failed", host + ":" + std::to_string(port)};
  }
  timeval tv{0, 200'000};  // wake regularly to notice stop()
  ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  socklen_t len = sizeof addr;
  ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  return static_cast<int>(ntohs(addr.sin_port));
}

void CoapServer::start() {
  running_ = true;
  thread_ = std::thread([this] { loop(); });
}

void CoapServer::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void CoapServer::loop() {
  std::vector<std::uint8_t> buf(2048);
  while (running_) {
    sockaddr_in from{};
    socklen_t flen = sizeof from;
    const ssize_t n =
        ::recvfrom(fd_, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &flen);
    if (n <= 0) continue;
    char ip[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
    const std::string peer = std::string(ip) + ":" + std::to_string(ntohs(from.sin_port));
    auto reply = handle({buf.begin(), buf.begin() + n}, peer);
    if (!reply.empty()) {
      ::sendto(fd_, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr*>(&from), flen);
    }
  }
}

bool CoapServer::allow(const std::string& device) {
  const auto now = std::chrono::steady_clock::now();
  auto [it, inserted] = buckets_.try_emplace(device, Bucket{burst_, now});
  Bucket& b = it->second;
  if (!inserted) {
    const double dt = std::chrono::duration<double>(now - b.last).count();
    b.tokens = std::min(burst_, b.tokens + dt * rate_);
    b.last = now;
  }
  if (b.tokens < 1.0) return false;
  b.tokens -= 1.0;
  return true;
}

std::vector<std::uint8_t> CoapServer::handle(const std::vector<std::uint8_t>& datagram,
                                             const std::string& peer) {
  coap::Message req;
  if (!coap::parse(datagram.data(), datagram.size(), req) ||
      (req.type != coap::Type::kCon && req.type != coap::Type::kNon)) {
    ++hub_.metrics().coap_bad_request;
    return {};  // not CoAP we answer: drop silently
  }
  for (const auto& c : cache_) {
    if (c.peer == peer && c.message_id == req.message_id) {
      ++hub_.metrics().coap_duplicate_messages;
      return c.reply;  // retransmitted CON: same answer, no reprocessing
    }
  }
  coap::Message rsp;
  rsp.type = req.type == coap::Type::kCon ? coap::Type::kAck : coap::Type::kNon;
  rsp.message_id = req.message_id;
  rsp.token_len = req.token_len;
  rsp.token = req.token;
  std::array<std::uint8_t, fw::wire::kMaxFrame> payload{};

  auto reply = [&](std::uint8_t code, std::size_t payload_len) {
    rsp.code = code;
    rsp.payload = payload_len ? payload.data() : nullptr;
    rsp.payload_len = payload_len;
    std::vector<std::uint8_t> out(kReplyCap);
    out.resize(coap::build(rsp, out.data(), out.size()));
    if (req.type == coap::Type::kCon) {
      cache_.push_back({peer, req.message_id, out});
      while (cache_.size() > kCacheSize) cache_.pop_front();
    }
    return out;
  };

  if (req.code != coap::kPost || req.path_count != 3 || req.segment(0) != "d" ||
      req.segment(2) != "e") {
    ++hub_.metrics().coap_bad_request;
    return reply(coap::kNotFound, 0);
  }
  const std::string device(req.segment(1));
  auto key_it = keys_.find(device);
  if (key_it == keys_.end()) {
    ++hub_.metrics().coap_auth_failed;
    return reply(coap::kUnauthorized, 0);
  }
  if (!allow(device)) {
    ++hub_.metrics().coap_rate_limited;
    return reply(coap::kTooManyRequests, 0);
  }
  const fw::wire::Key key{key_it->second.data(), key_it->second.size()};
  std::vector<fw::DeviceEvent> events;
  fw::wire::Tag request_tag{};
  const auto err = fw::wire::decode_uplink(req.payload, req.payload_len, key, &request_tag,
                                           [&](const fw::DeviceEvent& e) { events.push_back(e); });
  if (err == fw::wire::DecodeError::kBadTag) {
    ++hub_.metrics().coap_auth_failed;
    return reply(coap::kUnauthorized, 0);
  }
  if (err != fw::wire::DecodeError::kNone) {
    ++hub_.metrics().coap_bad_request;
    return reply(coap::kBadRequest, 0);
  }
  ++hub_.metrics().coap_frames_ok;
  const auto items = hub_.device_exchange(DeviceId(device), events, kDownlinkBudget);
  fw::wire::DownlinkWriter w(payload.data(), payload.size());
  for (const auto& d : items) {
    if (!w.add(d)) break;
  }
  const std::size_t n = w.finish(key, request_tag);
  return reply(coap::kChanged, n);
}

}  // namespace talos::server
