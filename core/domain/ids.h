// Strongly typed identifiers. Each ID kind is a distinct type so a DeviceId
// can never be passed where an AssetId is expected.
#pragma once

#include <compare>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace talos {

template <typename Tag>
class Id {
 public:
  Id() = default;
  explicit Id(std::string value) : value_(std::move(value)) {}

  const std::string& str() const { return value_; }
  bool empty() const { return value_.empty(); }
  auto operator<=>(const Id&) const = default;

 private:
  std::string value_;
};

using PropertyId = Id<struct PropertyIdTag>;
using ZoneId = Id<struct ZoneIdTag>;
using AssetId = Id<struct AssetIdTag>;
using DeviceId = Id<struct DeviceIdTag>;
using ModuleId = Id<struct ModuleIdTag>;  // module *type* in the catalog, e.g. "pir_ekmb_v1"
using CapabilityId = Id<struct CapabilityIdTag>;  // capability type, e.g. "motion.detect"
using EventId = Id<struct EventIdTag>;
using CommandId = Id<struct CommandIdTag>;
using PrincipalId = Id<struct PrincipalIdTag>;
using RuleId = Id<struct RuleIdTag>;
using BoardId = Id<struct BoardIdTag>;

}  // namespace talos

template <typename Tag>
struct std::hash<talos::Id<Tag>> {
  std::size_t operator()(const talos::Id<Tag>& id) const noexcept {
    return std::hash<std::string>{}(id.str());
  }
};
