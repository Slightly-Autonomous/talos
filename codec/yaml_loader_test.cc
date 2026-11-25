#include "codec/yaml_loader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "codec/codec.h"
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

TEST(YamlLoader, SampleDevicesDeclareOnlyWhatTheirModulesProvide) {
  auto property = load_property(kDir + "properties/sample_backyard.yaml").value();
  auto catalog = load_catalog();
  for (const char* extra : {"reed_contact_v1", "tmag5273a2_magnetic", "pir223d_motion"}) {
    auto m = load_module_manifest(kDir + "modules/" + extra + ".yaml").value();
    catalog.emplace(m.id, m);
  }
  const Device* node = property.find_device(DeviceId("node-07"));
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->board_revision, std::optional<std::string>("proto-a"));
  for (const auto* d : {node, property.find_device(DeviceId("node-12"))}) {
    ASSERT_FALSE(d->capabilities.empty());
    for (const auto& cap : d->capabilities) {
      // Board-level capabilities (the lid loop) come from the board manifest.
      // TL-SN-002 (sensor_node_v2) declares it too (ADR 0019).
      const std::string board_file = d->board == BoardId("sensor_node_v2")
                                         ? "boards/sensor_node_v2.yaml"
                                         : "boards/universal_controller_proto_a.yaml";
      const auto board_caps = load_board_manifest(kDir + board_file).value().board_capabilities;
      bool provided = std::find(board_caps.begin(), board_caps.end(), cap) != board_caps.end();
      for (const auto& slot : d->slots) {
        if (!slot || !catalog.contains(*slot)) continue;
        for (const auto& c : catalog.at(*slot).capabilities) provided |= c == cap;
      }
      EXPECT_TRUE(provided) << d->id.str() << " declares " << cap.str();
    }
  }
  EXPECT_EQ(property.find_device(DeviceId("lock-3"))->capabilities,
            std::vector<CapabilityId>{CapabilityId("access.lock")});
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
  EXPECT_EQ(b.reserved_i2c_addresses, (std::vector<std::uint8_t>{0x20, 0x21, 0x22, 0x6A, 0x70}));
  for (const auto& [id, m] : load_rev_a_catalog()) EXPECT_TRUE(m.provisional) << id.str();
}

// Third expander (practices review L-03), tamper (HD-032), EXP_WAKE (HD-034).
TEST(SensorNodeRevA, ExpandersWakeLinesAndIndicatorsAreDeclared) {
  auto b = sensor_node();
  ASSERT_EQ(b.expanders.size(), 3u);
  for (const auto& e : b.expanders) {
    EXPECT_EQ(e.pins.size(), 16u) << e.name;
    EXPECT_EQ(e.interrupt_net, "EXP_INT_N") << e.name;
    // Every expander's address is reserved: a card must never answer there.
    EXPECT_NE(
        std::find(b.reserved_i2c_addresses.begin(), b.reserved_i2c_addresses.end(), e.i2c_address),
        b.reserved_i2c_addresses.end())
        << e.name;
  }
  const auto& c = b.expanders[2];
  EXPECT_EQ(c.name, "EXP_C");
  EXPECT_EQ(c.i2c_address, 0x22);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(c.pins[i].net, "SLOT_LED" + std::to_string(i) + "_N");
    EXPECT_EQ(c.pins[i].direction, "output");
    EXPECT_TRUE(c.pins[i].active_low);
  }
  EXPECT_EQ(c.pins[4].net, "TAMPER_N");
  EXPECT_EQ(c.pins[4].direction, "input");
  for (std::size_t i = 5; i < 16; ++i) EXPECT_EQ(c.pins[i].direction, "unused") << c.pins[i].pin;
  // One ext1 ANY_HIGH mask: every wake line is active high, EXP_WAKE included.
  ASSERT_EQ(b.wake_lines.size(), 5u);
  for (const auto& w : b.wake_lines) EXPECT_TRUE(w.active_high) << w.net;
  EXPECT_EQ(b.wake_lines.back().net, "EXP_WAKE");
  EXPECT_EQ(b.wake_lines.back().mcu_pin, "GPIO14");
  ASSERT_EQ(b.indicators.size(), 6u);
  EXPECT_EQ(b.indicators[1].driver, "GPIO8");
  EXPECT_TRUE(b.indicators[1].active_low);
}

TEST(SensorNodeRevA, ExpanderPinDirectionIsChecked) {
  talos::v1::BoardManifest p;
  auto* e = p.add_expanders();
  e->set_name("EXP_X");
  auto* pin = e->add_pins();
  pin->set_pin("P00");
  pin->set_direction("sideways");
  EXPECT_FALSE(from_proto(p).ok());
  pin->set_direction("unused");
  EXPECT_TRUE(from_proto(p).ok());
}

// HW-MF-001: four reference cards fit on battery, USB and PV.
TEST(SensorNodeRevA, HW_MF_001_FourReferenceCardsFitEverySource) {
  // The T/RH card sits in a top slot (B-03).
  std::vector<SlotAssignment> a{{0, ModuleId("tl_mod_002_pir")},
                                {1, ModuleId("tl_mod_003_imu")},
                                {2, ModuleId("tl_mod_004_environment")},
                                {3, ModuleId("tl_mod_001_magnetic")}};
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
  for (SlotIndex i = 0; i < 4; ++i) a.push_back({i, ModuleId("tl_mod_003_imu")});
  EXPECT_TRUE(
      validate_configuration(sensor_node(), load_rev_a_catalog(), a, PowerSource::kUsb).ok());
}

// B-03: the T/RH card only in slots 0 and 2 on TL-SN-001; data from the board manifest.
TEST(SensorNodeRevA, B_03_EnvironmentCardOnlyInTheTopSlots) {
  auto catalog = load_rev_a_catalog();
  for (SlotIndex s = 0; s < 4; ++s) {
    auto r = validate_configuration(sensor_node(), catalog,
                                    {{s, ModuleId("tl_mod_004_environment")}}, PowerSource::kUsb);
    if (s == 0 || s == 2) {
      EXPECT_TRUE(r.ok()) << int(s);
    } else {
      ASSERT_TRUE(r.has(ManifestIssueCode::kSlotNotAllowed)) << int(s);
      EXPECT_EQ(r.issues.front().slot, std::optional<SlotIndex>{s});
      EXPECT_NE(r.issues.front().message.find("only allowed in slots 0, 2"), std::string::npos)
          << r.issues.front().message;
    }
  }
  // Other cards are unaffected, and the limit is the board's, not the module's.
  EXPECT_TRUE(validate_configuration(sensor_node(), catalog, {{1, ModuleId("tl_mod_003_imu")}},
                                     PowerSource::kUsb)
                  .ok());
  auto b = sensor_node();
  ASSERT_EQ(b.module_slot_limits.size(), 1u);
  b.module_slot_limits.clear();
  EXPECT_TRUE(validate_configuration(b, catalog, {{3, ModuleId("tl_mod_004_environment")}},
                                     PowerSource::kUsb)
                  .ok());
  EXPECT_EQ(b.board_capabilities, std::vector<CapabilityId>{CapabilityId("tamper.enclosure")});
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
  auto vn = load_board_manifest(kDir + "boards/vision_node_pi_zero2w.yaml");
  ASSERT_TRUE(vn.ok()) << vn.error().message;
  EXPECT_TRUE(vn.value().slots.empty());
  EXPECT_EQ(vn.value().transport, "ip");
  EXPECT_TRUE(validate_configuration(vn.value(), {}, {}, PowerSource::kPoe).ok());
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
// ---- TL-SN-002 (ADR 0019): built-in modules --------------------------------------------

ModuleCatalog load_sn002_catalog() {
  ModuleCatalog c;
  for (const char* name : {"tmag5273a2_magnetic", "vibration_lis2dw12_v1", "pir223d_motion",
                           "environment_sht40_v1", "tl_mod_003_imu"}) {
    auto m = load_module_manifest(kDir + "modules/" + name + ".yaml");
    EXPECT_TRUE(m.ok()) << name << ": " << m.error().message;
    if (m.ok()) c.emplace(m.value().id, m.value());
  }
  return c;
}

BoardManifest sensor_node_v2() {
  auto b = load_board_manifest(kDir + "boards/sensor_node_v2.yaml");
  EXPECT_TRUE(b.ok()) << b.error().message;
  return b.value();
}

TEST(SensorNodeV2, LoadsProvisionalWithBuiltInsBatteryAndNoSlotHardware) {
  auto b = sensor_node_v2();
  EXPECT_TRUE(b.provisional);
  EXPECT_EQ(b.board_family, "TL-SN-002");
  EXPECT_TRUE(b.connector_standard.empty());
  ASSERT_EQ(b.slots.size(), 4u);
  const char* want[] = {"tmag5273a2_magnetic", "vibration_lis2dw12_v1", "pir223d_motion",
                        "environment_sht40_v1"};
  for (std::size_t i = 0; i < 4; ++i) {
    ASSERT_TRUE(b.slots[i].built_in_module.has_value()) << i;
    EXPECT_EQ(b.slots[i].built_in_module->str(), want[i]);
    EXPECT_FALSE(b.slots[i].i2c_branch.has_value()) << "no mux";
  }
  ASSERT_TRUE(b.battery.has_value());
  EXPECT_EQ(b.battery->chemistry, "li-fes2");
  EXPECT_EQ(b.battery->cells_in_series, 2u);
  EXPECT_FALSE(b.battery->rechargeable);
  EXPECT_EQ(b.battery->low_mv, 2200u);
  EXPECT_EQ(b.wake_lines.size(), 5u);
  for (const auto& w : b.wake_lines) EXPECT_TRUE(w.active_high) << w.net;
  EXPECT_EQ(b.indicators.size(), 10u);
  EXPECT_EQ(b.board_capabilities,
            (std::vector<CapabilityId>{
                CapabilityId("tamper.enclosure"), CapabilityId("tamper.battery_door"),
                CapabilityId("tamper.mount"), CapabilityId("tamper.magnet")}));
  bool has_solar = false;
  for (const auto& p : b.power_sources) has_solar |= p.source == PowerSource::kSolarBattery;
  EXPECT_FALSE(has_solar) << "no PV input on TL-SN-002";
  for (const auto& [id, m] : load_sn002_catalog()) EXPECT_TRUE(m.provisional) << id.str();
}

TEST(SensorNodeV2, BuiltInsAreAlwaysPopulatedAndPassOnBothSources) {
  const auto b = sensor_node_v2();
  const auto catalog = load_sn002_catalog();
  // No assignment at all: the four built-ins are still there and fit.
  EXPECT_TRUE(validate_configuration(b, catalog, {}, PowerSource::kBatteryOnly).ok());
  EXPECT_TRUE(validate_configuration(b, catalog, {}, PowerSource::kUsb).ok());
  // Naming a built-in with its own module is fine (connector and bus checks skipped).
  EXPECT_TRUE(validate_configuration(b, catalog, {{2, ModuleId("pir223d_motion")}},
                                     PowerSource::kBatteryOnly)
                  .ok());
  EXPECT_FALSE(validate_configuration(b, catalog, {}, PowerSource::kSolarBattery).ok());
}

TEST(SensorNodeV2, ACardModuleCantReplaceABuiltIn) {
  const auto r = validate_configuration(sensor_node_v2(), load_sn002_catalog(),
                                        {{1, ModuleId("tl_mod_003_imu")}}, PowerSource::kUsb);
  EXPECT_TRUE(r.has(ManifestIssueCode::kBuiltInMismatch));
  EXPECT_FALSE(r.has(ManifestIssueCode::kConnectorMismatch));
}

TEST(SensorNodeV2, BuiltInsOnTheRootBusMustNotCollide) {
  auto b = sensor_node_v2();
  auto catalog = load_sn002_catalog();
  catalog.at(ModuleId("tmag5273a2_magnetic")).i2c_addresses = {0x44};  // a D-variant TMAG
  const auto r = validate_configuration(b, catalog, {}, PowerSource::kUsb);
  EXPECT_TRUE(r.has(ManifestIssueCode::kI2cAddressConflict));
  // And the board's own expander address is off limits.
  catalog = load_sn002_catalog();
  catalog.at(ModuleId("tmag5273a2_magnetic")).i2c_addresses = {0x20};
  EXPECT_TRUE(validate_configuration(b, catalog, {}, PowerSource::kUsb)
                  .has(ManifestIssueCode::kI2cRootConflict));
}

}  // namespace talos::codec
