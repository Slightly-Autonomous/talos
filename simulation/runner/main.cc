// talos_sim: run one scenario and print the result.
//   bazel run //simulation/runner:talos_sim -- <scenario.yaml> [--root DIR] [--trace OUT.json]
#include <cstdio>
#include <cstring>
#include <string>

#include "simulation/runner/runner.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: talos_sim <scenario.yaml> [--root DIR] [--trace OUT.json]\n");
    return 2;
  }
  std::string scenario = argv[1];
  std::string root = "simulation/scenarios";
  std::string trace;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--root") == 0) root = argv[i + 1];
    if (std::strcmp(argv[i], "--trace") == 0) trace = argv[i + 1];
  }
  auto report = talos::sim::run_scenario_file(scenario, root);
  if (!report.ok()) {
    std::fprintf(stderr, "error: %s\n", report.error().message.c_str());
    return 2;
  }
  const auto& r = report.value();
  std::printf("%s  %s  seed=%llu  journal=%llu entries  digest=%016llx\n",
              r.passed ? "PASS" : "FAIL", r.name.c_str(), static_cast<unsigned long long>(r.seed),
              static_cast<unsigned long long>(r.journal_entries),
              static_cast<unsigned long long>(r.journal_digest));
  for (const auto& f : r.failures) std::printf("  - %s\n", f.c_str());
  if (!trace.empty()) {
    if (auto s = talos::sim::write_text(trace, r.trace_json); !s) {
      std::fprintf(stderr, "could not write trace: %s\n", s.error().message.c_str());
    } else {
      std::printf("trace: %s\n", trace.c_str());
    }
  }
  return r.passed ? 0 : 1;
}
