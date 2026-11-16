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
  std::string base_dir;     // directory of the config file
  std::string sqlite_path;  // resolved against base_dir
};

// Loads YAML, resolves relative paths, validates, and builds the EngineConfig.
// Reserved switches (postgres storage, supabase auth, relay role) fail with a
// clear "reserved" error instead of silently falling back.
Result<LoadedConfig> load_config(const std::string& path);
Result<LoadedConfig> build_config(talos::server::v1::HubConfig hub, const std::string& base_dir);

}  // namespace talos::server
