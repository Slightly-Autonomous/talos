// Deterministic scenario runner (spec §42).
//
// Loads a scenario and its site, builds the production EngineConfig from
// them, drives the closed loop (world -> virtual sensors -> transport -> real
// HubEngine -> transport -> virtual actuators -> world) in fixed 100 ms
// virtual-time steps, then checks expectations and always-on invariants.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/base/result.h"
#include "talos/sim/v1/scenario.pb.h"
#include "talos/sim/v1/site.pb.h"

namespace talos::sim {

inline constexpr std::int64_t kStepMs = 100;

struct ScenarioReport {
  std::string name;
  std::uint64_t seed = 0;
  bool passed = false;
  std::vector<std::string> failures;
  std::uint64_t journal_entries = 0;
  std::uint64_t journal_digest = 0;  // FNV-1a over serialized journal entries
  std::string trace_json;            // ground truth + journal, for replay/debugging
};

ScenarioReport run_scenario(const talos::sim::v1::Scenario& scenario,
                            const talos::sim::v1::Site& site);

// Loads `scenario_path`; the scenario's `site` is resolved against `corpus_root`.
Result<ScenarioReport> run_scenario_file(const std::string& scenario_path,
                                         const std::string& corpus_root);

Status write_text(const std::string& path, const std::string& text);

}  // namespace talos::sim
