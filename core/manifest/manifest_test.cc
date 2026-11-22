#include "core/manifest/manifest.h"

#include <gtest/gtest.h>

namespace talos {
namespace {

// Illustrative, PROVISIONAL numbers only (not measured, not a schematic).
BoardManifest board() {
  BoardManifest b;
  b.id = BoardId("universal_controller_v1");
  b.revision = "proto-a";
  for (SlotIndex i = 0; i < 4; ++i) {
    SlotSpec s;
    s.index = i;
    s.wake = true;
    s.i2c_branch = i;  // one mux branch per slot
    s.spi = true;
    s.uart = (i == 2);  // only slot 2 has UART in this sketch
    s.rails = {"v3v3_aon", "v3v3_sw", "v5_sw"};
    b.slots.push_back(s);
  }
  b.rails = {{"v3v3_aon", 3300, 50, 20}, {"v3v3_sw", 3300, 300, 150}, {"v5_sw", 5000, 900, 700}};
  b.power_sources = {{PowerSource::kBatteryOnly, 15, 3000},
                     {PowerSource::kUsb, 2500, 4500},
                     {PowerSource::kSolarBattery, 300, 3000}};
  return b;
}

ModuleCatalog catalog() {
  ModuleCatalog c;
  auto add = [&](ModuleManifest m) { c.emplace(m.id, m); };
  ModuleManifest pir;
  pir.id = ModuleId("pir_ekmb_v1");
  pir.needs_wake = true;
  pir.loads = {{"v3v3_aon", 1}};
  pir.average_mw = 1;
  pir.peak_mw = 5;
  add(pir);
  ModuleManifest imu;
  imu.id = ModuleId("vibration_lis2dw12_v1");
  imu.needs_wake = true;
  imu.i2c_addresses = {0x19};
  imu.loads = {{"v3v3_aon", 1}};
  imu.average_mw = 1;
  imu.peak_mw = 3;
  add(imu);
  ModuleManifest cam;
  cam.id = ModuleId("smart_camera_s3_v1");
  cam.needs_uart = true;
  cam.loads = {{"v5_sw", 600}};
  cam.average_mw = 600;
  cam.peak_mw = 2500;
  add(cam);
  ModuleManifest env;
  env.id = ModuleId("environment_sht40_v1");
  env.i2c_addresses = {0x44};
  env.loads = {{"v3v3_sw", 1}};
  env.average_mw = 1;
  env.peak_mw = 2;
  add(env);
  return c;
}

std::vector<SlotAssignment> four_slot() {
  return {{0, ModuleId("pir_ekmb_v1")},
          {1, ModuleId("vibration_lis2dw12_v1")},
          {2, ModuleId("smart_camera_s3_v1")},
          {3, ModuleId("environment_sht40_v1")}};
}

TEST(ManifestValidator, FourSlotWithCameraPassesOnUsb) {
  auto r = validate_configuration(board(), catalog(), four_slot(), PowerSource::kUsb);
  EXPECT_TRUE(r.ok()) << (r.issues.empty() ? "" : r.issues[0].message);
}

TEST(ManifestValidator, SameCombinationRejectedOnBatteryOnly) {
  auto r = validate_configuration(board(), catalog(), four_slot(), PowerSource::kBatteryOnly);
  EXPECT_TRUE(r.has(ManifestIssueCode::kSustainedPowerExceeded));
  EXPECT_NE(r.issues[0].message.find("smart_camera_s3_v1"), std::string::npos);
}

TEST(ManifestValidator, LowPowerSensorsAloneFitOnBattery) {
  std::vector<SlotAssignment> a = {{0, ModuleId("pir_ekmb_v1")},
                                   {1, ModuleId("vibration_lis2dw12_v1")},
                                   {3, ModuleId("environment_sht40_v1")}};
  EXPECT_TRUE(validate_configuration(board(), catalog(), a, PowerSource::kBatteryOnly).ok());
}

TEST(ManifestValidator, UnsupportedPowerSource) {
  BoardManifest b = board();
  b.power_sources.pop_back();
  auto r = validate_configuration(b, catalog(), {}, PowerSource::kSolarBattery);
  EXPECT_TRUE(r.has(ManifestIssueCode::kPowerSourceUnsupported));
}

TEST(ManifestValidator, MissingWakeLine) {
  BoardManifest b = board();
  b.slots[0].wake = false;
  auto r = validate_configuration(b, catalog(), {{0, ModuleId("pir_ekmb_v1")}}, PowerSource::kUsb);
  ASSERT_TRUE(r.has(ManifestIssueCode::kMissingWake));
  EXPECT_EQ(r.issues[0].slot, SlotIndex{0});
}

TEST(ManifestValidator, CameraNeedsUartOnlySlotTwoHasIt) {
  auto r = validate_configuration(board(), catalog(), {{0, ModuleId("smart_camera_s3_v1")}},
                                  PowerSource::kUsb);
  EXPECT_TRUE(r.has(ManifestIssueCode::kMissingBus));
}

TEST(ManifestValidator, DuplicateI2cAddressOnlyConflictsOnSharedBranch) {
  std::vector<SlotAssignment> two_imus = {{0, ModuleId("vibration_lis2dw12_v1")},
                                          {1, ModuleId("vibration_lis2dw12_v1")}};
  EXPECT_TRUE(validate_configuration(board(), catalog(), two_imus, PowerSource::kUsb).ok());

  BoardManifest no_mux = board();
  for (auto& s : no_mux.slots) s.i2c_branch = 0;
  EXPECT_TRUE(validate_configuration(no_mux, catalog(), two_imus, PowerSource::kUsb)
                  .has(ManifestIssueCode::kI2cAddressConflict));
}

TEST(ManifestValidator, RailLimits) {
  BoardManifest b = board();
  b.rails[2].per_slot_max_ma = 500;  // v5_sw
  EXPECT_TRUE(
      validate_configuration(b, catalog(), {{2, ModuleId("smart_camera_s3_v1")}}, PowerSource::kUsb)
          .has(ManifestIssueCode::kRailSlotLimit));

  BoardManifest small_total = board();
  small_total.rails[2].max_ma = 400;
  EXPECT_TRUE(validate_configuration(small_total, catalog(), {{2, ModuleId("smart_camera_s3_v1")}},
                                     PowerSource::kUsb)
                  .has(ManifestIssueCode::kRailTotalLimit));

  BoardManifest no_5v = board();
  no_5v.slots[2].rails = {"v3v3_aon"};
  EXPECT_TRUE(validate_configuration(no_5v, catalog(), {{2, ModuleId("smart_camera_s3_v1")}},
                                     PowerSource::kUsb)
                  .has(ManifestIssueCode::kRailNotRouted));
}

TEST(ManifestValidator, UnknownModuleUnknownSlotDuplicateSlot) {
  auto r = validate_configuration(board(), catalog(),
                                  {{0, ModuleId("mystery_v9")},
                                   {7, ModuleId("pir_ekmb_v1")},
                                   {1, ModuleId("pir_ekmb_v1")},
                                   {1, ModuleId("pir_ekmb_v1")}},
                                  PowerSource::kUsb);
  EXPECT_TRUE(r.has(ManifestIssueCode::kUnknownModule));
  EXPECT_TRUE(r.has(ManifestIssueCode::kUnknownSlot));
  EXPECT_TRUE(r.has(ManifestIssueCode::kDuplicateSlot));
}

TEST(ManifestValidator, PeakBudget) {
  BoardManifest b = board();
  b.power_sources[1].peak_mw = 2000;
  EXPECT_TRUE(validate_configuration(b, catalog(), four_slot(), PowerSource::kUsb)
                  .has(ManifestIssueCode::kPeakPowerExceeded));
}

TEST(ModuleSlotLimits, ModuleOnlyInAllowedSlotsOthersUnaffected) {
  BoardManifest b = board();
  b.module_slot_limits = {{ModuleId("environment_sht40_v1"), {0, 2}, "away from the charger"}};
  auto run = [&](SlotIndex slot, const char* module) {
    return validate_configuration(b, catalog(), {{slot, ModuleId(module)}}, PowerSource::kUsb);
  };
  EXPECT_TRUE(run(0, "environment_sht40_v1").ok());
  EXPECT_TRUE(run(2, "environment_sht40_v1").ok());
  const auto r = run(3, "environment_sht40_v1");
  ASSERT_TRUE(r.has(ManifestIssueCode::kSlotNotAllowed));
  EXPECT_EQ(r.issues.front().message,
            "environment_sht40_v1 is only allowed in slots 0, 2 on universal_controller_v1 (away "
            "from the charger)");
  EXPECT_EQ(to_string(ManifestIssueCode::kSlotNotAllowed), "slot_not_allowed");
  EXPECT_TRUE(run(3, "vibration_lis2dw12_v1").ok());
}

}  // namespace
// Red team (ADR 0019): built-in modules keep the wake check, and a card on a mux
// branch can't reuse a built-in's root-bus address (enabled branches join root).
TEST(Validation, BuiltInWakeAndRootAddressAgainstCards) {
  BoardManifest b = board();
  b.slots[0].built_in_module = ModuleId("vibration_lis2dw12_v1");  // 0x19, needs wake
  b.slots[0].i2c_branch.reset();
  EXPECT_TRUE(validate_configuration(b, catalog(), {}, PowerSource::kUsb).ok());
  b.slots[0].wake = false;
  EXPECT_TRUE(validate_configuration(b, catalog(), {}, PowerSource::kUsb)
                  .has(ManifestIssueCode::kMissingWake));
  b.slots[0].wake = true;
  // A second LIS2DW12 on a card behind the mux: 0x19 on a branch collides with root.
  const auto r = validate_configuration(b, catalog(), {{1, ModuleId("vibration_lis2dw12_v1")}},
                                        PowerSource::kUsb);
  EXPECT_TRUE(r.has(ManifestIssueCode::kI2cRootConflict));
  // A card with another address is fine.
  EXPECT_FALSE(validate_configuration(b, catalog(), {{1, ModuleId("environment_sht40_v1")}},
                                      PowerSource::kUsb)
                   .has(ManifestIssueCode::kI2cRootConflict));
}

}  // namespace talos
