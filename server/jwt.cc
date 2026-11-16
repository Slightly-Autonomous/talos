#include "server/jwt.h"

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include "common/sha256/sha256.h"

namespace talos::server {

namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::optional<google::protobuf::Struct> parse_json(const std::string& s) {
  google::protobuf::Struct out;
  if (!google::protobuf::util::JsonStringToMessage(s, &out).ok()) return std::nullopt;
  return out;
}

std::optional<double> number(const google::protobuf::Struct& s, const char* key) {
  auto it = s.fields().find(key);
  if (it == s.fields().end() || !it->second.has_number_value()) return std::nullopt;
  return it->second.number_value();
}

std::string str(const google::protobuf::Struct& s, const char* key) {
  auto it = s.fields().find(key);
  return it != s.fields().end() && it->second.has_string_value() ? it->second.string_value() : "";
}
}  // namespace

std::string base64url_encode(std::string_view data) {
  std::string out;
  std::uint32_t buf = 0;
  int bits = 0;
  for (unsigned char c : data) {
    buf = (buf << 8) | c;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out += kAlphabet[(buf >> bits) & 0x3F];
    }
  }
  if (bits > 0) out += kAlphabet[(buf << (6 - bits)) & 0x3F];
  return out;
}

std::optional<std::string> base64url_decode(std::string_view in) {
  std::string out;
  std::uint32_t buf = 0;
  int bits = 0;
  for (char c : in) {
    int v;
    if (c >= 'A' && c <= 'Z')
      v = c - 'A';
    else if (c >= 'a' && c <= 'z')
      v = c - 'a' + 26;
    else if (c >= '0' && c <= '9')
      v = c - '0' + 52;
    else if (c == '-')
      v = 62;
    else if (c == '_')
      v = 63;
    else if (c == '=')
      break;
    else
      return std::nullopt;
    buf = (buf << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((buf >> bits) & 0xFF);
    }
  }
  return out;
}

std::optional<JwtClaims> verify_hs256(std::string_view token, std::string_view secret,
                                      std::string_view audience, std::int64_t now_s) {
  if (secret.empty()) return std::nullopt;
  const auto d1 = token.find('.');
  const auto d2 = d1 == std::string_view::npos ? d1 : token.find('.', d1 + 1);
  if (d2 == std::string_view::npos || token.find('.', d2 + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  auto header_json = base64url_decode(token.substr(0, d1));
  auto payload_json = base64url_decode(token.substr(d1 + 1, d2 - d1 - 1));
  auto sig = base64url_decode(token.substr(d2 + 1));
  if (!header_json || !payload_json || !sig || sig->size() != 32) return std::nullopt;
  auto header = parse_json(*header_json);
  if (!header || str(*header, "alg") != "HS256") return std::nullopt;  // never "none"

  talos::HmacSha256 mac(reinterpret_cast<const std::uint8_t*>(secret.data()), secret.size());
  const auto signing_input = token.substr(0, d2);
  mac.update(reinterpret_cast<const std::uint8_t*>(signing_input.data()), signing_input.size());
  const auto expected = mac.finish();
  if (!talos::constant_time_equal(expected.data(),
                                  reinterpret_cast<const std::uint8_t*>(sig->data()), 32)) {
    return std::nullopt;
  }
  auto payload = parse_json(*payload_json);
  if (!payload) return std::nullopt;
  const auto exp = number(*payload, "exp");
  if (!exp || static_cast<std::int64_t>(*exp) <= now_s) return std::nullopt;
  if (auto nbf = number(*payload, "nbf"); nbf && static_cast<std::int64_t>(*nbf) > now_s) {
    return std::nullopt;
  }
  bool aud_ok = false;
  if (auto it = payload->fields().find("aud"); it != payload->fields().end()) {
    if (it->second.has_string_value()) aud_ok = it->second.string_value() == audience;
    if (it->second.has_list_value()) {
      for (const auto& v : it->second.list_value().values()) aud_ok |= v.string_value() == audience;
    }
  }
  const std::string sub = str(*payload, "sub");
  if (!aud_ok || sub.empty()) return std::nullopt;
  return JwtClaims{sub, static_cast<std::int64_t>(*exp)};
}

}  // namespace talos::server
