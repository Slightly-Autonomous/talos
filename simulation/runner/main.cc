// talos_sim: run one scenario and print the result.
//   bazel run //simulation/runner:talos_sim -- <scenario.yaml> [--root DIR] [--trace OUT.json]
//       [--backend behavioural|firmware]
// --backend firmware runs every device the portable firmware can play on the virtual board
// (L2) and the rest on the behavioural model (L1); site `binding:` overrides it per device.
// Exit: 0 pass, 1 fail, 2 error, 3 skipped (a device is bound to a backend that cannot run).
#include <cstdio>
#include <cstring>
#include <string>

#include "simulation/runner/runner.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: talos_sim <scenario.yaml> [--root DIR] [--trace OUT.json] "
                 "[--backend behavioural|firmware]\n");
    return 2;
  }
  std::string scenario = argv[1];
  std::string root = "simulation/scenarios";
  std::string trace;
  talos::sim::RunOptions options;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--root") == 0) root = argv[i + 1];
    if (std::strcmp(argv[i], "--trace") == 0) trace = argv[i + 1];
    if (std::strcmp(argv[i], "--backend") == 0) {
      const std::string b = argv[i + 1];
      if (b == "firmware" || b == "l2") {
        options.backend = talos::sim::Backend::kFirmware;
      } else if (b != "behavioural" && b != "l1") {
        std::fprintf(stderr, "unknown backend %s (behavioural|firmware)\n", b.c_str());
        return 2;
      }
    }
  }
  auto report = talos::sim::run_scenario_file(scenario, root, options);
  if (!report.ok()) {
    std::fprintf(stderr, "error: %s\n", report.error().message.c_str());
    return 2;
  }
  const auto& r = report.value();
  if (r.skipped) {
    std::printf("SKIP  %s  %s\n", r.name.c_str(), r.skip_reason.c_str());
    return 3;
  }
  std::printf("%s  %s  seed=%llu  journal=%llu entries  digest=%016llx\n",
              r.passed ? "PASS" : "FAIL", r.name.c_str(), static_cast<unsigned long long>(r.seed),
              static_cast<unsigned long long>(r.journal_entries),
              static_cast<unsigned long long>(r.journal_digest));
  std::printf("  backends:");
  for (const auto& [device, level] : r.bindings)
    std::printf(" %s=%s", device.c_str(), level.c_str());
  std::printf("\n");
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
