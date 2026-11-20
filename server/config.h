// Hub configuration loading and validation (ADR 0010).
#pragma once

#include <string>

#include "core/base/result.h"
#include "core/engine/engine.h"
#include "talos/server/v1/config.pb.h"
#include "talos/v1/property.pb.h"

namespace talos::server {

struct LoadedConfig {
  talos::server::v1::HubConfig hub;
  talos::v1::Property property_proto;
  EngineConfig engine;
  // YAML principals before rule principals are added; reused on every
  // config revision so rule principals track the current rules.
  std::vector<Principal> base_principals;
  // Revision 1 when the config store is empty (ADR 0012).
  talos::server::v1::ConfigDocument bootstrap_document;
  std::string base_dir;     // directory of the config file
  std::string sqlite_path;  // resolved against base_dir
};

// Loads YAML, resolves relative paths, validates, and builds the EngineConfig.
// Reserved switches (postgres storage, supabase auth, relay role) fail with a
// clear "reserved" error instead of silently falling back.
Result<LoadedConfig> load_config(const std::string& path);

struct ParsedDocument {
  Property property;
  std::vector<Rule> rules;
};

// Validates an editable config document: property integrity, rules target
// this property, rule zones and action targets exist, and every action agrees
// with the operation catalog (args, target capability, TTL, expectation).
Result<ParsedDocument> parse_document(const talos::server::v1::ConfigDocument& doc,
                                      const PropertyId& expected_property);

// Adds a kRule principal (disruptive_output ceiling) for each distinct rule
// principal id. A rule principal id that names a configured principal is an
// error: a rule never acts as a person, agent or system identity.
Result<std::vector<Principal>> with_rule_principals(std::vector<Principal> base,
                                                    const std::vector<Rule>& rules,
                                                    const PropertyId& property);
// dev_mode configs carry well-known tokens and device keys: refuse a
// non-loopback HTTP host, and a non-loopback CoAP host when CoAP is enabled,
// unless the operator explicitly allows it (TALOS_ALLOW_DEV_ON_NETWORK=1).
Status check_dev_mode_hosts(const talos::server::v1::HubConfig& hub, const std::string& http_host,
                            bool allow_dev_on_network);

Result<LoadedConfig> build_config(talos::server::v1::HubConfig hub, const std::string& base_dir);

}  // namespace talos::server
