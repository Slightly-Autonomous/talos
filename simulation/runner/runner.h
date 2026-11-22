// Deterministic scenario runner (spec §42).
//
// Loads a scenario and its site, builds the production EngineConfig from
// them, drives the closed loop (world -> virtual sensors -> transport -> real
// HubEngine -> transport -> virtual actuators -> world) in fixed 100 ms
// virtual-time steps, then checks expectations and always-on invariants.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/base/result.h"
#include "talos/sim/v1/scenario.pb.h"
#include "talos/sim/v1/site.pb.h"

namespace talos::sim {

inline constexpr std::int64_t kStepMs = 100;

// Which twin backend plays devices whose site binding is unspecified (ADR 0018).
enum class Backend {
  kBehavioural,  // L1 for every device
  kFirmware,     // L2 where the firmware can play the device, L1 for the rest
};

struct RunOptions {
  Backend backend = Backend::kBehavioural;
};

struct ScenarioReport {
  std::string name;
  std::uint64_t seed = 0;
  bool passed = false;
  // Not run: a device is bound to a backend that cannot play it here. Never a pass.
  bool skipped = false;
  std::string skip_reason;
  // device id -> "L1" / "L2", in device order.
  std::vector<std::pair<std::string, std::string>> bindings;
  std::vector<std::string> failures;
  std::uint64_t journal_entries = 0;
  std::uint64_t journal_digest = 0;  // FNV-1a over serialized journal entries
  // "device event_type" -> journaled observations (the parity test compares these).
  std::map<std::string, std::uint32_t> observations;
  // "device kind" -> diagnostics the devices sent (observability plane, e.g.
  // "console.locked"); the parity test compares these too.
  std::map<std::string, std::uint32_t> diagnostics;
  std::string trace_json;  // ground truth + journal, for replay/debugging
};

ScenarioReport run_scenario(const talos::sim::v1::Scenario& scenario,
                            const talos::sim::v1::Site& site, RunOptions options = {});

// Loads `scenario_path`; the scenario's `site` is resolved against `corpus_root`.
Result<ScenarioReport> run_scenario_file(const std::string& scenario_path,
                                         const std::string& corpus_root, RunOptions options = {});

Status write_text(const std::string& path, const std::string& text);

}  // namespace talos::sim
