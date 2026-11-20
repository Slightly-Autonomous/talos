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

// ---- TL-SN-001 Rev A manifests (hardware handoff §38, HW-MF-*) ---------------------

ModuleCatalog load_rev_a_catalog() {
  ModuleCatalog c;
  for (const char* name : {"tl_mod_001_magnetic", "tl_mod_002_pir", "tl_mod_003_imu",
                           "tl_mod_004_environment", "tl_test_001", "pir_ekmb_v1"}) {
    auto m = load_module_manifest(kDir + "modules/" + name + ".yaml");
    EXPECT_TRUE(m.ok()) << name << ": " << m.error().message;
    if (m.ok()) c.emplace(m.value().id, m.value());
  }
  return c;
}

BoardManifest sensor_node() {
  auto b = load_board_manifest(kDir + "boards/sensor_node_proto_a.yaml");
  EXPECT_TRUE(b.ok()) << b.error().message;
  return b.value();
}

TEST(SensorNodeRevA, ManifestsLoadProvisionalWithFamilyAndConnector) {
  auto b = sensor_node();
  EXPECT_TRUE(b.provisional);
  EXPECT_EQ(b.board_family, "TL-SN-001");
  EXPECT_EQ(b.transport, "thread");
  EXPECT_EQ(b.connector_standard, "sensor_edge_v1");
  EXPECT_EQ(b.uart_channels, 1u);
  EXPECT_EQ(b.reserved_i2c_addresses, (std::vector<std::uint8_t>{0x20, 0x21, 0x6A, 0x70}));
  for (const auto& [id, m] : load_rev_a_catalog()) EXPECT_TRUE(m.provisional) << id.str();
}

// HW-MF-001: four reference cards fit on battery, USB and PV.
TEST(SensorNodeRevA, HW_MF_001_FourReferenceCardsFitEverySource) {
  std::vector<SlotAssignment> a{{0, ModuleId("tl_mod_002_pir")},
                                {1, ModuleId("tl_mod_003_imu")},
                                {2, ModuleId("tl_mod_001_magnetic")},
                                {3, ModuleId("tl_mod_004_environment")}};
  auto catalog = load_rev_a_catalog();
  for (auto src : {PowerSource::kBatteryOnly, PowerSource::kUsb, PowerSource::kSolarBattery}) {
    auto r = validate_configuration(sensor_node(), catalog, a, src);
    EXPECT_TRUE(r.ok()) << to_string(src) << ": "
                        << (r.issues.empty() ? "" : r.issues.front().message);
  }
}

// HW-MF-002: every card has an ID EEPROM at 0x50; isolated branches make that legal.
TEST(SensorNodeRevA, HW_MF_002_SameEepromAddressOnEveryBranchIsFine) {
  std::vector<SlotAssignment> a;
  for (SlotIndex i = 0; i < 4; ++i) a.push_back({i, ModuleId("tl_mod_004_environment")});
  EXPECT_TRUE(
      validate_configuration(sensor_node(), load_rev_a_catalog(), a, PowerSource::kUsb).ok());
}

// HW-MF-003: unisolated (shared) branch with the same address is rejected.
TEST(SensorNodeRevA, HW_MF_003_SharedBranchConflictRejected) {
  auto b = sensor_node();
  b.slots[1].i2c_branch = 0;  // pretend slots 0 and 1 share a branch
  std::vector<SlotAssignment> a{{0, ModuleId("tl_mod_003_imu")}, {1, ModuleId("tl_mod_003_imu")}};
  EXPECT_TRUE(validate_configuration(b, load_rev_a_catalog(), a, PowerSource::kUsb)
                  .has(ManifestIssueCode::kI2cAddressConflict));
}

// HW-MF-004: wake-required card on a slot without a wake path.
TEST(SensorNodeRevA, HW_MF_004_WakeRequiredOnSlotWithoutWake) {
  auto b = sensor_node();
  b.slots[3].wake = false;
  std::vector<SlotAssignment> a{{3, ModuleId("tl_mod_002_pir")}};
  EXPECT_TRUE(validate_configuration(b, load_rev_a_catalog(), a, PowerSource::kUsb)
                  .has(ManifestIssueCode::kMissingWake));
}

// HW-MF-005: two UART cards but one muxed UART.
TEST(SensorNodeRevA, HW_MF_005_TwoUartCardsOneUart) {
  auto catalog = load_rev_a_catalog();
  ModuleManifest radar;
  radar.id = ModuleId("uart_radar_card");
  radar.connector_standard = "sensor_edge_v1";
  radar.needs_uart = true;
  radar.i2c_addresses = {0x50};
  radar.loads = {{"v3v3_aon", 1}};
  catalog.emplace(radar.id, radar);
  std::vector<SlotAssignment> a{{0, radar.id}, {1, radar.id}};
  auto r = validate_configuration(sensor_node(), catalog, a, PowerSource::kUsb);
  EXPECT_TRUE(r.has(ManifestIssueCode::kUartOversubscribed));
  std::vector<SlotAssignment> one{{0, radar.id}};
  EXPECT_TRUE(validate_configuration(sensor_node(), catalog, one, PowerSource::kUsb).ok());
}

// HW-MF-005: four TL-TEST-001 fixtures are allowed (the UART is time-shared),
// but a fixture still counts nothing toward a field card's UART.
TEST(SensorNodeRevA, HW_MF_005_TestFixturesAreExemptFromTheUartLimit) {
  const auto catalog = load_rev_a_catalog();
  ASSERT_TRUE(catalog.at(ModuleId("tl_test_001")).test_fixture);
  std::vector<SlotAssignment> four;
  for (SlotIndex s = 0; s < 4; ++s) four.push_back({s, ModuleId("tl_test_001")});
  const auto r = validate_configuration(sensor_node(), catalog, four, PowerSource::kUsb);
  EXPECT_FALSE(r.has(ManifestIssueCode::kUartOversubscribed));
  EXPECT_TRUE(r.ok());
}

// HW-MF-006: four high-current cards exceed the battery budget but fit on USB.
TEST(SensorNodeRevA, HW_MF_006_HighCurrentCardsRejectedOnBattery) {
  auto catalog = load_rev_a_catalog();
  ModuleManifest hungry;
  hungry.id = ModuleId("radar_class_card");
  hungry.connector_standard = "sensor_edge_v1";
  hungry.loads = {{"v3v3_sw", 70}};
  hungry.average_mw = 200;
  hungry.peak_mw = 190;
  catalog.emplace(hungry.id, hungry);
  std::vector<SlotAssignment> a;
  for (SlotIndex i = 0; i < 4; ++i) a.push_back({i, hungry.id});
  auto battery = validate_configuration(sensor_node(), catalog, a, PowerSource::kBatteryOnly);
  EXPECT_TRUE(battery.has(ManifestIssueCode::kSustainedPowerExceeded));
  EXPECT_TRUE(validate_configuration(sensor_node(), catalog, a, PowerSource::kUsb).ok());
}

// HW-MF-007: card for another connector revision is never activated.
TEST(SensorNodeRevA, HW_MF_007_LegacyCardConnectorMismatch) {
  std::vector<SlotAssignment> a{{0, ModuleId("pir_ekmb_v1")}};  // pre-sensor_edge_v1 manifest
  EXPECT_TRUE(validate_configuration(sensor_node(), load_rev_a_catalog(), a, PowerSource::kUsb)
                  .has(ManifestIssueCode::kConnectorMismatch));
}

// HW-MF-008: a card asking for 5 V keeps it disabled (Rev A routes no 5 V rail).
TEST(SensorNodeRevA, HW_MF_008_FiveVoltCardRejected) {
  auto catalog = load_rev_a_catalog();
  ModuleManifest five;
  five.id = ModuleId("needs_5v");
  five.connector_standard = "sensor_edge_v1";
  five.loads = {{"v5_sw", 50}};
  catalog.emplace(five.id, five);
  std::vector<SlotAssignment> a{{0, five.id}};
  EXPECT_TRUE(validate_configuration(sensor_node(), catalog, a, PowerSource::kUsb)
                  .has(ManifestIssueCode::kRailNotRouted));
}

// HW-MF-009: Vision Node with zero slots is valid on its own power sources.
TEST(SensorNodeRevA, HW_MF_009_VisionNodeZeroSlotsValid) {
  auto vn = load_board_manifest(kDir + "boards/vision_node_proto_a.yaml");
  ASSERT_TRUE(vn.ok()) << vn.error().message;
  EXPECT_TRUE(vn.value().slots.empty());
  EXPECT_EQ(vn.value().transport, "ip");
  EXPECT_TRUE(validate_configuration(vn.value(), {}, {}, PowerSource::kUsb).ok());
  EXPECT_TRUE(validate_configuration(vn.value(), {}, {}, PowerSource::kBatteryOnly)
                  .has(ManifestIssueCode::kPowerSourceUnsupported));
}

// Cards must avoid the board's root-bus addresses (mux branches join the root bus).
TEST(SensorNodeRevA, RootAddressConflictRejected) {
  auto catalog = load_rev_a_catalog();
  ModuleManifest clash;
  clash.id = ModuleId("expander_at_0x20");
  clash.connector_standard = "sensor_edge_v1";
  clash.i2c_addresses = {0x20};
  catalog.emplace(clash.id, clash);
  std::vector<SlotAssignment> a{{2, clash.id}};
  EXPECT_TRUE(validate_configuration(sensor_node(), catalog, a, PowerSource::kUsb)
                  .has(ManifestIssueCode::kI2cRootConflict));
}

}  // namespace
}  // namespace talos::codec
