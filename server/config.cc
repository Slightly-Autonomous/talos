#include "server/config.h"

#include <filesystem>
#include <set>

#include "codec/codec.h"
#include "codec/yaml_loader.h"

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
      return reserved("auth: AUTH_MODE_SUPABASE");
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
  for (const auto& rule : e.rules) {
    if (!ids.contains(rule.principal.str())) {
      e.principals.push_back(
          {rule.principal, PrincipalKind::kRule, {{e.property.id, RiskClass::kDisruptiveOutput}}});
      ids.insert(rule.principal.str());
    }
  }
  std::set<std::string> device_tokens;
  for (const auto& d : hub.devices()) {
    if (!e.property.find_device(DeviceId(d.device_id()))) {
      return invalid("device credential for unknown device " + d.device_id());
    }
    if (!is_sha256_hex(d.token_sha256())) {
      return invalid("device " + d.device_id() + ": token_sha256 must be 64 lowercase hex chars");
    }
    if (!device_tokens.insert(d.token_sha256()).second) {
      return invalid("device tokens must be unique (" + d.device_id() + ")");
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

Result<LoadedConfig> load_config(const std::string& path) {
  v1::HubConfig hub;
  if (Status s = codec::load_yaml_file(path, hub); !s) return s.error();
  return build_config(std::move(hub), fs::absolute(path).parent_path().string());
}

}  // namespace talos::server
