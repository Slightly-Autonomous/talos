#include "server/config.h"

#include <filesystem>
#include <set>

#include "codec/codec.h"
#include "codec/yaml_loader.h"
#include "core/execution/operations.h"

namespace talos::server {

namespace fs = std::filesystem;

namespace {

Error invalid(std::string msg) { return {"invalid_config", std::move(msg)}; }
Error reserved(std::string what) {
  return {"reserved_option", what + " is reserved and not implemented yet (see ADR 0010)"};
}

bool is_sha256_hex(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

RiskClass to_risk(talos::v1::RiskClass r) {
  switch (r) {
    case talos::v1::RISK_CLASS_SENSITIVE_READ:
      return RiskClass::kSensitiveRead;
    case talos::v1::RISK_CLASS_BENIGN_OUTPUT:
      return RiskClass::kBenignOutput;
    case talos::v1::RISK_CLASS_DISRUPTIVE_OUTPUT:
      return RiskClass::kDisruptiveOutput;
    case talos::v1::RISK_CLASS_SECURITY_CRITICAL:
      return RiskClass::kSecurityCritical;
    default:
      return RiskClass::kInformational;
  }
}

std::string resolve(const std::string& base, const std::string& p) {
  if (p.empty() || fs::path(p).is_absolute()) return p;
  return (fs::path(base) / p).lexically_normal().string();
}

}  // namespace

Result<LoadedConfig> build_config(v1::HubConfig hub, const std::string& base_dir) {
  switch (hub.role()) {
    case v1::HUB_ROLE_UNSPECIFIED:
    case v1::HUB_ROLE_HUB:
      break;
    case v1::HUB_ROLE_RELAY:
      return reserved("role: HUB_ROLE_RELAY");
    default:
      return invalid("unknown role");
  }
  switch (hub.storage().driver()) {
    case v1::STORAGE_DRIVER_UNSPECIFIED:
    case v1::STORAGE_DRIVER_SQLITE:
      break;
    case v1::STORAGE_DRIVER_POSTGRES:
      return reserved("storage.driver: STORAGE_DRIVER_POSTGRES");
    default:
      return invalid("unknown storage driver");
  }
  switch (hub.auth()) {
    case v1::AUTH_MODE_UNSPECIFIED:
    case v1::AUTH_MODE_LOCAL:
      break;
    case v1::AUTH_MODE_SUPABASE:
      if (hub.supabase_auth().jwt_secret_env().empty()) {
        return invalid("auth SUPABASE needs supabase_auth.jwt_secret_env");
      }
      break;
    default:
      return invalid("unknown auth mode");
  }
  if (hub.sync().target() == v1::SYNC_TARGET_SUPABASE &&
      (hub.sync().supabase_url_env().empty() || hub.sync().supabase_service_key_env().empty())) {
    return invalid("sync.target SUPABASE needs supabase_url_env and supabase_service_key_env");
  }

  LoadedConfig out;
  out.base_dir = base_dir;
  out.sqlite_path =
      resolve(base_dir, hub.storage().sqlite_path().empty() ? "talos-journal.db"
                                                            : hub.storage().sqlite_path());
  if (hub.property_file().empty()) return invalid("property_file is required");
  if (Status s = codec::load_yaml_file(resolve(base_dir, hub.property_file()), out.property_proto);
      !s) {
    return s.error();
  }
  auto property = codec::from_proto(out.property_proto);
  if (!property) return property.error();
  if (Status s = validate_property(property.value()); !s) return s.error();

  EngineConfig& e = out.engine;
  e.property = property.value();
  e.freshness_max_age =
      Millis{hub.freshness_max_age_ms() > 0 ? hub.freshness_max_age_ms() : 15 * 60 * 1000};
  for (const auto& r : hub.rules()) {
    auto rule = codec::from_proto(r);
    if (!rule) return rule.error();
    if (rule.value().property != e.property.id) {
      return invalid("rule " + rule.value().id.str() + " targets another property");
    }
    e.rules.push_back(rule.value());
  }

  std::set<std::string> ids;
  for (const auto& p : hub.principals()) {
    if (p.id().empty() || !ids.insert(p.id()).second) {
      return invalid("principal ids must be unique and non-empty: '" + p.id() + "'");
    }
    if (!p.token_sha256().empty() && !is_sha256_hex(p.token_sha256())) {
      return invalid("principal " + p.id() + ": token_sha256 must be 64 lowercase hex chars");
    }
    PrincipalKind kind = PrincipalKind::kHuman;
    switch (p.kind()) {
      case v1::PRINCIPAL_KIND_AGENT:
        kind = PrincipalKind::kAgent;
        break;
      case v1::PRINCIPAL_KIND_RULE:
        kind = PrincipalKind::kRule;
        break;
      case v1::PRINCIPAL_KIND_SYSTEM:
        kind = PrincipalKind::kSystem;
        break;
      case v1::PRINCIPAL_KIND_HUMAN:
        break;
      default:
        return invalid("principal " + p.id() + ": kind is required");
    }
    e.principals.push_back({PrincipalId(p.id()), kind, {{e.property.id, to_risk(p.max_risk())}}});
  }
  out.base_principals = e.principals;
  auto with_rules = with_rule_principals(out.base_principals, e.rules, e.property.id);
  if (!with_rules) return with_rules.error();
  e.principals = std::move(with_rules).value();
  *out.bootstrap_document.mutable_property() = out.property_proto;
  *out.bootstrap_document.mutable_rules() = hub.rules();
  if (auto parsed = parse_document(out.bootstrap_document, e.property.id); !parsed) {
    return parsed.error();
  }
  for (const auto& u : hub.supabase_auth().users()) {
    if (u.user_id().empty() || !ids.contains(u.principal_id())) {
      return invalid("supabase user '" + u.user_id() + "' maps to unknown principal '" +
                     u.principal_id() + "'");
    }
  }
  // One hash, one identity: a hash shared between credentials would make the
  // authenticator silently pick one of them.
  std::set<std::string> token_hashes;
  for (const auto& p : hub.principals()) {
    if (!p.token_sha256().empty() && !token_hashes.insert(p.token_sha256()).second) {
      return invalid("token hashes must be unique across credentials (" + p.id() + ")");
    }
  }
  for (const auto& d : hub.devices()) {
    if (!e.property.find_device(DeviceId(d.device_id()))) {
      return invalid("device credential for unknown device " + d.device_id());
    }
    if (!is_sha256_hex(d.token_sha256())) {
      return invalid("device " + d.device_id() + ": token_sha256 must be 64 lowercase hex chars");
    }
    if (!token_hashes.insert(d.token_sha256()).second) {
      return invalid("token hashes must be unique across credentials (" + d.device_id() + ")");
    }
  }

  const std::int64_t offset = hub.utc_offset_minutes();
  e.local_minute_of_day = [offset](Timestamp t) -> std::uint16_t {
    std::int64_t m = (t.unix_ms / 60'000 + offset) % 1440;
    if (m < 0) m += 1440;
    return static_cast<std::uint16_t>(m);
  };
  out.hub = std::move(hub);
  return out;
}

Result<std::vector<Principal>> with_rule_principals(std::vector<Principal> base,
                                                    const std::vector<Rule>& rules,
                                                    const PropertyId& property) {
  const std::size_t configured = base.size();
  for (const auto& rule : rules) {
    for (std::size_t i = 0; i < configured; ++i) {
      if (base[i].id == rule.principal) {
        return invalid("rule " + rule.id.str() + " uses principal '" + rule.principal.str() +
                       "', which is a configured principal; rules act under their own id");
      }
    }
    bool present = false;
    for (std::size_t i = configured; i < base.size(); ++i) present |= base[i].id == rule.principal;
    if (!present) {
      base.push_back(
          {rule.principal, PrincipalKind::kRule, {{property, RiskClass::kDisruptiveOutput}}});
    }
  }
  return base;
}

Result<ParsedDocument> parse_document(const v1::ConfigDocument& doc,
                                      const PropertyId& expected_property) {
  auto property = codec::from_proto(doc.property());
  if (!property) return property.error();
  if (property.value().id != expected_property) {
    return invalid("document is for property '" + property.value().id.str() + "', expected '" +
                   expected_property.str() + "'");
  }
  if (Status s = validate_property(property.value()); !s) return s.error();
  ParsedDocument out{property.value(), {}};
  std::set<std::string> rule_ids;
  for (const auto& r : doc.rules()) {
    auto rule = codec::from_proto(r);
    if (!rule) return rule.error();
    const Rule& ru = rule.value();
    if (ru.id.empty() || !rule_ids.insert(ru.id.str()).second) {
      return invalid("rule ids must be unique and non-empty: '" + ru.id.str() + "'");
    }
    if (ru.property != expected_property) {
      return invalid("rule " + ru.id.str() + " targets another property");
    }
    if (ru.principal.empty()) return invalid("rule " + ru.id.str() + " needs a principal_id");
    if (ru.trigger.event_type.empty()) return invalid("rule " + ru.id.str() + " needs a trigger");
    if (ru.trigger.zone && !out.property.find_zone(*ru.trigger.zone)) {
      return invalid("rule " + ru.id.str() + " uses unknown zone " + ru.trigger.zone->str());
    }
    for (const auto& a : ru.actions) {
      const Device* device = out.property.find_device(a.target.device);
      if (!device) {
        return invalid("rule " + ru.id.str() + " targets unknown device " + a.target.device.str());
      }
      if (!device->declares(a.target.capability)) {
        return invalid("rule " + ru.id.str() + " targets " + a.target.capability.str() +
                       ", which " + a.target.device.str() + " does not declare");
      }
      // The engine re-checks at run time; refusing here keeps a rule that
      // could never be accepted out of the active configuration.
      Command shape;
      shape.operation = a.operation;
      shape.target = a.target;
      shape.args = a.args;
      shape.expires_at = Timestamp{} + a.ttl;
      shape.verification = a.verification;
      shape.verification_window = a.verification_window;
      shape.expect = a.expect;
      if (auto n = normalize_command(shape); !n) {
        return invalid("rule " + ru.id.str() + " action " + a.operation + ": " + n.error().code +
                       " (" + n.error().message + ")");
      }
    }
    out.rules.push_back(ru);
  }
  return out;
}

namespace {

// Token hashes and device keys published in deploy/config (hub.dev.yaml,
// dev-device-keys.yaml). Anyone can read them, so they are never accepted on
// a network-facing hub, whatever dev_mode says (red team #16).
constexpr const char* kKnownDevTokenHashes[] = {
    "8ac4afa44f3f01da2d47daaee53270af09cdf9b0482481377b750332904ddd94",  // owner
    "d967787d3023c82861961e4fb7acaacf48d1175fa1873b4b679bec86c69afd7c",  // mcp-agent
    "a069201469e8a354bbd72208f9377a2f6c78d76b83a8fde56e763072dc7f42d8",  // metrics
    "a5272572495630914856ca946d2c9868fb9cfce8fc1a674e411989cdd4c5a1bc",  // sync-worker
    "3a0c053a94f5d03270628729fd2f84ba52065698f1b95719409e93042df778a3",  // node-07
    "65060935b3f93197b2100a6d3137aefa655a8961fe06f9dbade7a5bc86cb988f",  // node-12
    "d53ebf5b31fe00c0381b5ec2dcd95a1981018ddde6064236be8eed5720fb720d",  // patio_speaker
    "dd63e701464b4a0530f6b22693a920496c64086aab7ff8f045db279a1b28948f",  // lock-3
};
constexpr const char* kKnownDevDeviceKeys[] = {
    "000102030405060708090a0b0c0d0e0f",
    "101112131415161718191a1b1c1d1e1f",
};

bool loopback(const std::string& h) { return h == "127.0.0.1" || h == "localhost" || h == "::1"; }

std::string hex_of(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (auto b : bytes) {
    out += kHex[b >> 4];
    out += kHex[b & 15];
  }
  return out;
}

}  // namespace

bool is_known_dev_token_hash(const std::string& sha256_hex) {
  for (const char* h : kKnownDevTokenHashes) {
    if (sha256_hex == h) return true;
  }
  return false;
}

bool is_known_dev_device_key(const std::vector<std::uint8_t>& key) {
  const std::string hex = hex_of(key);
  for (const char* k : kKnownDevDeviceKeys) {
    if (hex == k) return true;
  }
  return false;
}

Status check_dev_mode_hosts(const v1::HubConfig& hub, const std::string& http_host,
                            bool allow_dev_on_network,
                            const std::vector<std::vector<std::uint8_t>>& device_keys) {
  if (allow_dev_on_network) return Status::Ok();
  std::string why;
  if (hub.dev_mode()) why = "config has dev_mode=true (well-known dev tokens and keys)";
  for (const auto& p : hub.principals()) {
    if (why.empty() && is_known_dev_token_hash(p.token_sha256())) {
      why = "principal " + p.id() + " uses a published dev token";
    }
  }
  for (const auto& d : hub.devices()) {
    if (why.empty() && is_known_dev_token_hash(d.token_sha256())) {
      why = "device " + d.device_id() + " uses a published dev token";
    }
  }
  for (const auto& k : device_keys) {
    if (why.empty() && is_known_dev_device_key(k)) why = "a CoAP device key is a published dev key";
  }
  if (why.empty()) return Status::Ok();
  std::vector<std::string> hosts{http_host};
  if (hub.coap().enabled()) {
    hosts.push_back(hub.coap().listen_host().empty() ? http_host : hub.coap().listen_host());
  }
  for (const auto& h : hosts) {
    if (!loopback(h)) {
      return Error{"dev_mode_on_network", "refusing to listen on " + h + ": " + why + "."};
    }
  }
  return Status::Ok();
}

Result<LoadedConfig> load_config(const std::string& path) {
  v1::HubConfig hub;
  if (Status s = codec::load_yaml_file(path, hub); !s) return s.error();
  return build_config(std::move(hub), fs::absolute(path).parent_path().string());
}

}  // namespace talos::server
