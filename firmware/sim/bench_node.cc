// The bench console on a virtual TL-SN-001.
//
//   bench_node [--slot0=TL-MOD-004] ... [--fault=branch_open2] ...
//
// Reads one command per line from stdin and answers exactly as the ESP32 does
// over USB-serial-JTAG ("@T {json}" lines, then a done line). Lines starting
// with "sim " control the simulation instead:
//   sim fault <name>   inject a fault (firmware/sim/sim_faults.h)
//   sim run <ms>       advance virtual time
//   sim boot           reset the MCU (expanders and cards keep their state)
// Between commands the node runs for 20 ms of virtual time.
#include <cstdio>
#include <iostream>
#include <string>

#include "firmware/portable/diag.h"
#include "firmware/sim/sim_faults.h"
#include "firmware/sim/virtual_board.h"

namespace {

class StdoutOut final : public talos::fw::IDiagOut {
 public:
  void write(const char* s, std::size_t n) override {
    std::fwrite(s, 1, n, stdout);
    std::fflush(stdout);
  }
};

void sim_done(const char* cmd, bool ok, const char* err = nullptr) {
  std::printf("@T {\"cmd\":\"%s\",\"done\":true,\"ok\":%s%s%s%s}\n", cmd, ok ? "true" : "false",
              err ? ",\"err\":\"" : "", err ? err : "", err ? "\"" : "");
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace talos::fw;
  sim::Node node(sim::tl_sn_001_board());
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--slot", 0) == 0 && arg.size() > 8 && arg[7] == '=') {
      const int slot = arg[6] - '0';
      ModuleCode code{};
      if (slot < 0 || slot >= kSlots || !sim::parse_card(arg.substr(8), code)) {
        std::fprintf(stderr, "bad %s\n", arg.c_str());
        return 2;
      }
      node.insert(static_cast<std::uint8_t>(slot), code);
    }
  }
  for (int i = 1; i < argc; ++i) {  // faults after the cards exist
    const std::string arg = argv[i];
    if (arg.rfind("--fault=", 0) == 0 && !sim::apply_fault(node, arg.substr(8))) {
      std::fprintf(stderr, "unknown fault %s\n", arg.c_str());
      return 2;
    }
  }
  node.boot();
  node.run_for(20);
  StdoutOut out;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.rfind("sim ", 0) == 0) {
      const std::string rest = line.substr(4);
      if (rest.rfind("fault ", 0) == 0) {
        const bool ok = sim::apply_fault(node, rest.substr(6));
        sim_done("sim", ok, ok ? nullptr : "unknown fault");
      } else if (rest.rfind("run ", 0) == 0) {
        node.run_for(std::stoull(rest.substr(4)));
        sim_done("sim", true);
      } else if (rest == "boot") {
        node.boot();
        sim_done("sim", true);
      } else {
        sim_done("sim", false, "unknown sim command");
      }
      continue;
    }
    Diag diag(node.platform(), node.runtime(), node.has_board_io() ? &node.board_io() : nullptr);
    diag.handle_line(line.c_str(), out);
    node.run_for(20);
  }
  return 0;
}
