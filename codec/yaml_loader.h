// Loads YAML configuration files as protobuf messages.
//
// YAML is converted to JSON and parsed with the protobuf JSON mapping, so the
// .proto files stay the single schema. Plain YAML scalars are typed (bool,
// number); quoted scalars are always strings. Unknown keys are errors.
#pragma once

#include <google/protobuf/message.h>

#include <string>

#include "core/base/result.h"
#include "core/domain/property.h"
#include "core/manifest/manifest.h"

namespace talos::codec {

// Parses YAML text into `out`.
Status parse_yaml(const std::string& yaml_text, google::protobuf::Message& out);
Status load_yaml_file(const std::string& path, google::protobuf::Message& out);

Result<BoardManifest> load_board_manifest(const std::string& path);
Result<ModuleManifest> load_module_manifest(const std::string& path);
Result<Property> load_property(const std::string& path);

}  // namespace talos::codec
