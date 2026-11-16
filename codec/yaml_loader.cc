#include "codec/yaml_loader.h"

#include <google/protobuf/util/json_util.h>
#include <yaml-cpp/yaml.h>

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "codec/codec.h"

namespace talos::codec {

namespace {

void append_json_string(const std::string& s, std::string& out) {
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

bool is_json_number(const std::string& s) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  std::strtod(s.c_str(), &end);
  return errno == 0 && end == s.c_str() + s.size() && s.find_first_of("xXnN") == std::string::npos;
}

Status to_json(const YAML::Node& node, std::string& out) {
  switch (node.Type()) {
    case YAML::NodeType::Null:
      out += "null";
      return Status::Ok();
    case YAML::NodeType::Scalar: {
      const std::string& v = node.Scalar();
      const bool quoted = node.Tag() == "!";
      if (!quoted && (v == "true" || v == "false")) {
        out += v;
      } else if (!quoted && is_json_number(v)) {
        out += v;
      } else {
        append_json_string(v, out);
      }
      return Status::Ok();
    }
    case YAML::NodeType::Sequence: {
      out += '[';
      bool first = true;
      for (const auto& item : node) {
        if (!first) out += ',';
        first = false;
        if (Status s = to_json(item, out); !s) return s;
      }
      out += ']';
      return Status::Ok();
    }
    case YAML::NodeType::Map: {
      out += '{';
      bool first = true;
      for (const auto& kv : node) {
        if (!first) out += ',';
        first = false;
        append_json_string(kv.first.as<std::string>(), out);
        out += ':';
        if (Status s = to_json(kv.second, out); !s) return s;
      }
      out += '}';
      return Status::Ok();
    }
    case YAML::NodeType::Undefined:
      break;
  }
  return Error{"invalid_yaml", "undefined node"};
}

}  // namespace

Status parse_yaml(const std::string& yaml_text, google::protobuf::Message& out) {
  YAML::Node root;
  try {  // yaml-cpp reports errors by exception; contained to this boundary.
    root = YAML::Load(yaml_text);
  } catch (const YAML::Exception& e) {
    return Error{"invalid_yaml", e.what()};
  }
  std::string json;
  if (Status s = to_json(root, json); !s) return s;
  google::protobuf::util::JsonParseOptions opts;
  opts.ignore_unknown_fields = false;
  auto st = google::protobuf::util::JsonStringToMessage(json, &out, opts);
  if (!st.ok()) return Error{"schema_mismatch", std::string(st.message())};
  return Status::Ok();
}

Status load_yaml_file(const std::string& path, google::protobuf::Message& out) {
  std::ifstream in(path);
  if (!in) return Error{"file_not_found", path};
  std::stringstream ss;
  ss << in.rdbuf();
  if (Status s = parse_yaml(ss.str(), out); !s) {
    return Error{s.error().code, path + ": " + s.error().message};
  }
  return Status::Ok();
}

template <typename Proto, typename Core>
Result<Core> load_as(const std::string& path) {
  Proto p;
  if (Status s = load_yaml_file(path, p); !s) return s.error();
  return from_proto(p);
}

Result<BoardManifest> load_board_manifest(const std::string& path) {
  return load_as<talos::v1::BoardManifest, BoardManifest>(path);
}
Result<ModuleManifest> load_module_manifest(const std::string& path) {
  return load_as<talos::v1::ModuleManifest, ModuleManifest>(path);
}
Result<Property> load_property(const std::string& path) {
  return load_as<talos::v1::Property, Property>(path);
}

}  // namespace talos::codec
