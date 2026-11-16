// Runs every scenario in the corpus through the real core, twice: it must pass
// and both runs must produce byte-identical journals. Failures write a full
// trace (seed, ground truth, journal) to the Bazel undeclared-outputs dir.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "codec/yaml_loader.h"
#include "simulation/runner/runner.h"

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

std::string trace_dir() {
  const char* d = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  return d ? d : ".";
}

class Scenario : public ::testing::TestWithParam<std::string> {};

TEST_P(Scenario, PassesDeterministically) {
  const std::string path = kRoot + "/scenarios/" + GetParam() + ".yaml";
  auto first = run_scenario_file(path, kRoot);
  ASSERT_TRUE(first.ok()) << first.error().message;
  const auto& r = first.value();
  if (!r.passed) {
    const std::string trace = trace_dir() + "/" + GetParam() + ".trace.json";
    (void)write_text(trace, r.trace_json);
    std::string why;
    for (const auto& f : r.failures) why += "\n  - " + f;
    ADD_FAILURE() << GetParam() << " failed (seed " << r.seed << ")" << why
                  << "\n  trace: " << trace
                  << "\n  rerun: bazel run //simulation/runner:talos_sim -- $PWD/" << path
                  << " --root $PWD/" << kRoot;
  }
  auto second = run_scenario_file(path, kRoot);
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(second.value().journal_digest, r.journal_digest) << "nondeterministic journal";
  EXPECT_GT(r.journal_entries, 0u);
}

INSTANTIATE_TEST_SUITE_P(Corpus, Scenario, ::testing::ValuesIn(corpus()),
                         [](const auto& info) { return info.param; });

TEST(Corpus, HasAtLeastTwentyScenarios) { EXPECT_GE(corpus().size(), 20u); }

// Metamorphic: shifting the whole scenario by a day (same local time of day)
// yields the same journal shape and outcome.
TEST(Metamorphic, DayShiftPreservesOutcome) {
  v1::Scenario sc;
  ASSERT_TRUE(codec::load_yaml_file(kRoot + "/scenarios/backyard_intrusion_at_2am.yaml", sc));
  v1::Site site;
  ASSERT_TRUE(codec::load_yaml_file(kRoot + "/" + sc.site(), site));
  const auto base = run_scenario(sc, site);
  sc.set_start_unix_ms(sc.start_unix_ms() + 24LL * 3600 * 1000);
  const auto shifted = run_scenario(sc, site);
  EXPECT_TRUE(base.passed);
  EXPECT_TRUE(shifted.passed);
  EXPECT_EQ(base.journal_entries, shifted.journal_entries);
}

// Metamorphic: a different seed may change noise but not the outcome of a
// scenario without randomness-sensitive expectations.
TEST(Metamorphic, SeedChangeKeepsNoiseFreeOutcome) {
  v1::Scenario sc;
  ASSERT_TRUE(codec::load_yaml_file(kRoot + "/scenarios/backyard_intrusion_at_2am.yaml", sc));
  v1::Site site;
  ASSERT_TRUE(codec::load_yaml_file(kRoot + "/" + sc.site(), site));
  for (std::uint64_t seed : {1u, 2u, 3u, 99u}) {
    sc.set_seed(seed);
    EXPECT_TRUE(run_scenario(sc, site).passed) << "seed " << seed;
  }
}

}  // namespace
}  // namespace talos::sim
