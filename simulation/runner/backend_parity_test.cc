// L1/L2 parity (bench-test-plan §3, ADR 0018): every scenario runs on the behavioural model
// (L1) and again with every device the firmware can play on the real portable firmware over
// the virtual board (L2). Both must pass, and each L2 device must report the same domain
// transitions as its L1 model. A difference means the behavioural model claims something the
// firmware does not do (or the reverse), and the L1 corpus can no longer be trusted for it.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "codec/yaml_loader.h"
#include "simulation/runner/runner.h"
#include "simulation/twin/firmware_device.h"

namespace talos::sim {
namespace {

const std::string kRoot = "simulation/scenarios";

std::vector<std::string> corpus() {
  std::vector<std::string> out;
  for (const auto& e : std::filesystem::directory_iterator(kRoot + "/scenarios")) {
    if (e.path().extension() == ".yaml") out.push_back(e.path().stem().string());
  }
  std::sort(out.begin(), out.end());
  return out;
}

// Transitions a device must agree on. Heartbeats, boot state reports and environment
// readings follow each backend's own schedule and are covered by the scenarios' expectations.
bool compared(const std::string& event_type) {
  static const std::set<std::string> kTransitions = {
      "motion.detected", "motion.cleared", "contact.opened", "contact.closed",
      "impact.detected", "tamper.opened",  "tamper.closed"};
  return kTransitions.contains(event_type);
}

std::string why(const ScenarioReport& r) {
  std::string out = r.skip_reason;
  for (const auto& f : r.failures) out += "\n  - " + f;
  return out;
}

class Parity : public ::testing::TestWithParam<std::string> {};

TEST_P(Parity, FirmwareMatchesBehaviouralModel) {
  const std::string path = kRoot + "/scenarios/" + GetParam() + ".yaml";
  auto l1 = run_scenario_file(path, kRoot, {Backend::kBehavioural});
  auto l2 = run_scenario_file(path, kRoot, {Backend::kFirmware});
  ASSERT_TRUE(l1.ok() && l2.ok());
  ASSERT_TRUE(l1.value().passed) << "L1: " << why(l1.value());
  if (l2.value().skipped) GTEST_SKIP() << l2.value().skip_reason;
  EXPECT_TRUE(l2.value().passed) << "L2: " << why(l2.value());

  std::set<std::string> firmware_devices;
  for (const auto& [device, level] : l2.value().bindings) {
    if (level == "L2") firmware_devices.insert(device);
  }
  ASSERT_FALSE(firmware_devices.empty());
  std::set<std::string> keys;
  for (const auto* r : {&l1.value(), &l2.value()}) {
    for (const auto& [key, n] : r->observations) keys.insert(key);
  }
  for (const auto& key : keys) {
    const auto space = key.find(' ');
    const std::string device = key.substr(0, space);
    if (!firmware_devices.contains(device) || !compared(key.substr(space + 1))) continue;
    auto count = [&](const ScenarioReport& r) {
      auto it = r.observations.find(key);
      return it == r.observations.end() ? 0u : it->second;
    };
    EXPECT_EQ(count(l2.value()), count(l1.value())) << key << " (L2 firmware vs L1 model)";
  }
  // Diagnostics both backends model (the USB console lock's rate-limited reports).
  std::set<std::string> diag_keys;
  for (const auto* r : {&l1.value(), &l2.value()}) {
    for (const auto& [key, n] : r->diagnostics) diag_keys.insert(key);
  }
  for (const auto& key : diag_keys) {
    const auto space = key.find(' ');
    if (!firmware_devices.contains(key.substr(0, space))) continue;
    auto count = [&](const ScenarioReport& r) {
      auto it = r.diagnostics.find(key);
      return it == r.diagnostics.end() ? 0u : it->second;
    };
    EXPECT_EQ(count(l2.value()), count(l1.value())) << key << " (L2 firmware vs L1 model)";
  }
}

INSTANTIATE_TEST_SUITE_P(Corpus, Parity, ::testing::ValuesIn(corpus()),
                         [](const auto& info) { return info.param; });

// Most of the corpus must actually exercise firmware, or parity proves nothing.
TEST(ParityCoverage, MostScenariosRunFirmware) {
  int with_firmware = 0;
  for (const auto& name : corpus()) {
    auto r = run_scenario_file(kRoot + "/scenarios/" + name + ".yaml", kRoot, {Backend::kFirmware});
    ASSERT_TRUE(r.ok());
    with_firmware += r.value().skipped ? 0 : 1;
  }
  EXPECT_GE(with_firmware, 20);
}

// --- binding rules ---------------------------------------------------------------------------

struct Fixture {
  v1::Scenario sc;
  v1::Site site;
  Fixture() {
    EXPECT_TRUE(codec::load_yaml_file(kRoot + "/scenarios/front_door_open_close.yaml", sc));
    EXPECT_TRUE(codec::load_yaml_file(kRoot + "/" + sc.site(), site));
  }
  v1::SiteDevice& device(const std::string& id) {
    for (auto& d : *site.mutable_devices()) {
      if (d.id() == id) return d;
    }
    ADD_FAILURE() << "no device " << id;
    return *site.mutable_devices(0);
  }
};

TEST(Binding, DefaultRunIsAllBehavioural) {
  Fixture f;
  const auto r = run_scenario(f.sc, f.site);
  ASSERT_FALSE(r.skipped);
  for (const auto& [device, level] : r.bindings) EXPECT_EQ(level, "L1") << device;
}

TEST(Binding, ExplicitFirmwareBindingRunsFirmwareOnAnL1Run) {
  Fixture f;
  f.device("node_front").set_binding(v1::DEVICE_BINDING_FIRMWARE);
  const auto r = run_scenario(f.sc, f.site);
  EXPECT_TRUE(r.passed) << why(r);
  for (const auto& [device, level] : r.bindings) {
    EXPECT_EQ(level, device == "node_front" ? "L2" : "L1") << device;
  }
}

TEST(Binding, ExplicitBehaviouralBindingWinsOverFirmwareRun) {
  Fixture f;
  f.device("node_front").set_binding(v1::DEVICE_BINDING_BEHAVIOURAL);
  const auto r = run_scenario(f.sc, f.site, {Backend::kFirmware});
  for (const auto& [device, level] : r.bindings) {
    if (device == "node_front") {
      EXPECT_EQ(level, "L1");
    }
  }
}

TEST(Binding, FirmwareBoundCameraIsSkippedWithTheReason) {
  Fixture f;
  f.device("camera_backyard").set_binding(v1::DEVICE_BINDING_FIRMWARE);
  const auto r = run_scenario(f.sc, f.site);
  EXPECT_TRUE(r.skipped);
  EXPECT_FALSE(r.passed);
  EXPECT_NE(r.skip_reason.find("no camera"), std::string::npos) << r.skip_reason;
}

TEST(Binding, HardwareBindingIsSkippedNotPassed) {
  Fixture f;
  f.device("node_front").set_binding(v1::DEVICE_BINDING_HARDWARE);
  const auto r = run_scenario(f.sc, f.site);
  EXPECT_TRUE(r.skipped);
  EXPECT_FALSE(r.passed);
  EXPECT_NE(r.skip_reason.find("L4"), std::string::npos) << r.skip_reason;
}

TEST(Binding, FirmwareRunWithNothingFirmwareCanPlayIsSkipped) {
  Fixture f;
  // Keep only the actuators, which the firmware cannot play.
  auto* devices = f.site.mutable_devices();
  for (int i = devices->size() - 1; i >= 0; --i) {
    const auto& d = devices->Get(i);
    if (!d.has_lock() && !d.has_speaker() && !d.has_notifier()) devices->DeleteSubrange(i, 1);
  }
  f.sc.clear_expect();
  const auto r = run_scenario(f.sc, f.site, {Backend::kFirmware});
  EXPECT_TRUE(r.skipped);
  EXPECT_NE(r.skip_reason.find("no device"), std::string::npos) << r.skip_reason;
}

// Scenario start finds devices already warm; a reboot is a real power-on. The firmware masks
// the PIR for its warm-up (60 s on TL-SN-002), so motion 6 s after a reboot is not reported on L2.
TEST(Binding, RebootedFirmwareNodeIsBlindDuringPirWarmup) {
  v1::Scenario sc;
  ASSERT_TRUE(codec::load_yaml_file(kRoot + "/scenarios/garage_side_door_intrusion.yaml", sc));
  v1::Site site;
  ASSERT_TRUE(codec::load_yaml_file(kRoot + "/" + sc.site(), site));
  ASSERT_TRUE(run_scenario(sc, site, {Backend::kFirmware}).passed);
  auto* reboot = sc.add_stimuli();
  reboot->set_at_ms(500);
  reboot->mutable_device_reboot()->set_device_id("node_garage");
  const auto r = run_scenario(sc, site, {Backend::kFirmware});
  EXPECT_FALSE(r.passed);
  EXPECT_EQ(r.observations.count("node_garage motion.detected"), 0u);
}

}  // namespace
}  // namespace talos::sim

namespace talos::sim {
namespace {

// ADR 0019: the garage and driveway nodes are TL-SN-002 and run on its L2 board model
// (built-in sensors), while the split-off driveway camera stays L1.
TEST(Binding, GarageNodesRunOnTheTlSn002Model) {
  auto r = run_scenario_file(kRoot + "/scenarios/garage_side_door_intrusion.yaml", kRoot,
                             {Backend::kFirmware});
  ASSERT_TRUE(r.ok());
  ASSERT_TRUE(r.value().passed);
  std::map<std::string, std::string> level(r.value().bindings.begin(), r.value().bindings.end());
  EXPECT_EQ(level["node_garage"], "L2");
  EXPECT_EQ(level["node_driveway"], "L2");
  EXPECT_EQ(level["cam_driveway"], "L1");
}

// A TL-SN-002 slot only takes its own built-in module.
TEST(Binding, Sn002RefusesAModuleThatIsNotBuiltIn) {
  v1::SiteDevice d;
  d.set_id("n");
  d.set_board_id("sensor_node_v2");
  auto* s = d.add_sensors();
  s->set_slot(0);
  s->set_module_id("reed_contact_v1");
  s->mutable_contact()->set_portal_id("p");
  EXPECT_NE(FirmwareDevice::unsupported_reason(d), "");
  s->set_module_id("tmag5273a2_magnetic");
  EXPECT_EQ(FirmwareDevice::unsupported_reason(d), "");
}

}  // namespace
}  // namespace talos::sim
