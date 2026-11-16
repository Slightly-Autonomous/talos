#include "codec/yaml_loader.h"

#include <gtest/gtest.h>

#include "core/manifest/manifest.h"
#include "talos/v1/manifest.pb.h"

namespace talos::codec {
namespace {

const std::string kDir = "contracts/manifests/";

ModuleCatalog load_catalog() {
  ModuleCatalog c;
  for (const char* name :
       {"pir_ekmb_v1", "vibration_lis2dw12_v1", "smart_camera_s3_v1", "environment_sht40_v1"}) {
    auto m = load_module_manifest(kDir + "modules/" + name + ".yaml");
    EXPECT_TRUE(m.ok()) << m.error().message;
    if (m.ok()) c.emplace(m.value().id, m.value());
  }
  return c;
}

TEST(YamlLoader, SampleManifestsLoadAndAreMarkedProvisional) {
  auto board = load_board_manifest(kDir + "boards/universal_controller_proto_a.yaml");
  ASSERT_TRUE(board.ok()) << board.error().message;
  EXPECT_TRUE(board.value().provisional);
  EXPECT_EQ(board.value().slots.size(), 4u);
  for (const auto& [id, m] : load_catalog()) EXPECT_TRUE(m.provisional) << id.str();
}

TEST(YamlLoader, SampleFourSlotNodeValidatesAgainstPowerSource) {
  auto board = load_board_manifest(kDir + "boards/universal_controller_proto_a.yaml").value();
  auto property = load_property(kDir + "properties/sample_backyard.yaml");
  ASSERT_TRUE(property.ok()) << property.error().message;
  ASSERT_TRUE(validate_property(property.value()));

  const Device* node = property.value().find_device(DeviceId("node-07"));
  ASSERT_NE(node, nullptr);
  std::vector<SlotAssignment> assignments;
  for (std::size_t i = 0; i < node->slots.size(); ++i) {
    if (node->slots[i]) assignments.push_back({static_cast<SlotIndex>(i), *node->slots[i]});
  }
  ASSERT_EQ(assignments.size(), 4u);
  auto catalog = load_catalog();
  EXPECT_TRUE(validate_configuration(board, catalog, assignments, PowerSource::kUsb).ok());
  EXPECT_TRUE(validate_configuration(board, catalog, assignments, PowerSource::kBatteryOnly)
                  .has(ManifestIssueCode::kSustainedPowerExceeded));
}

TEST(YamlLoader, UnknownKeysAndBadTypesAreErrors) {
  talos::v1::ModuleManifest m;
  EXPECT_EQ(parse_yaml("id: x\nmystery_field: 3\n", m).error().code, "schema_mismatch");
  EXPECT_EQ(parse_yaml("id: x\nneeds_wake: [1, 2]\n", m).error().code, "schema_mismatch");
  EXPECT_EQ(parse_yaml("id: [unterminated\n", m).error().code, "invalid_yaml");
}

TEST(YamlLoader, QuotedNumbersStayStrings) {
  talos::v1::ModuleManifest m;
  ASSERT_TRUE(parse_yaml("id: \"007\"\nrevision: '2'\naverage_mw: 5\n", m));
  EXPECT_EQ(m.id(), "007");
  EXPECT_EQ(m.revision(), "2");
  EXPECT_EQ(m.average_mw(), 5u);
}

TEST(YamlLoader, MissingFile) {
  EXPECT_EQ(load_board_manifest("does/not/exist.yaml").error().code, "file_not_found");
}

}  // namespace
}  // namespace talos::codec
