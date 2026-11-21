// The bench console on a virtual TL-SN-001, behaving like the board on its USB port.
//
//   bench_node [--slot0=TL-MOD-004] ... [--fault=branch_open2] ... [--provision=node-12:<hex key>]
//              [--board-id=TL-SN-001:A:SIM-0001 | --no-board-id]
//
// The board identity (D-09) is programmed as on a finished board unless
// --no-board-id leaves it blank (the state before `talos_dbg program-id --node`).
//
// stdin/stdout carry exactly what the ESP32's USB-serial-JTAG carries: diag commands in,
// "@T {json}" replies out, and CoAP datagrams both ways as "@F <base64>" lines (only once the
// node is provisioned, as on hardware). Lines starting "sim " control the simulation:
//   sim fault <name>   inject a fault (firmware/sim/sim_faults.h)
//   sim run <ms>       advance virtual time now
//   sim boot           reset the MCU (expanders and cards keep their state)
//   sim deepsleep      the node sleeps, then wakes if an armed ext1 line is high now
//                      (EXP_WAKE, slot WAKE): a deep-sleep wake with those lines.
//                      Replies ok with no wake if nothing would have woken it.
// Virtual time follows wall time in 10 ms steps, so a host bridge sees a live node.
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "firmware/portable/diag.h"
#include "firmware/portable/serial_link.h"
#include "firmware/sim/sim_faults.h"
#include "firmware/sim/virtual_board.h"

namespace {

using namespace talos::fw;

class StdoutOut final : public IDiagOut, public ILineSink {
 public:
  void write(const char* s, std::size_t n) override {
    std::fwrite(s, 1, n, stdout);
    std::fflush(stdout);
  }
};

// The node's link is rebuilt at every boot from the stored identity, like the ESP32 does.
class RebuildableLink final : public ILink {
 public:
  bool send(const DeviceEvent& e) override { return link ? link->send(e) : false; }
  std::optional<Downlink> receive() override { return link ? link->receive() : std::nullopt; }
  std::unique_ptr<ProvisionedLink> link;
};

void sim_done(bool ok, const char* err = nullptr) {
  std::printf("@T {\"cmd\":\"sim\",\"done\":true,\"ok\":%s%s%s%s}\n", ok ? "true" : "false",
              err ? ",\"err\":\"" : "", err ? err : "", err ? "\"" : "");
  std::fflush(stdout);
}

bool provision_from_arg(IStore& store, const std::string& spec) {
  const auto colon = spec.find(':');
  if (colon == std::string::npos || colon == 0 || colon > DeviceIdentity::kMaxId) return false;
  DeviceIdentity id;
  std::memcpy(id.device_id, spec.data(), colon);
  const std::string hex = spec.substr(colon + 1);
  if (hex.size() % 2 || hex.size() / 2 > DeviceIdentity::kMaxKey) return false;
  for (std::size_t i = 0; i < hex.size() / 2; ++i) {
    id.key[i] = static_cast<std::uint8_t>(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
  }
  id.key_len = static_cast<std::uint8_t>(hex.size() / 2);
  return save_identity(store, id);
}

bool board_id_from_arg(IStore& store, const std::string& spec) {
  const auto a = spec.find(':');
  const auto b = a == std::string::npos ? a : spec.find(':', a + 1);
  if (b == std::string::npos) return false;
  const std::string board = spec.substr(0, a), rev = spec.substr(a + 1, b - a - 1),
                    serial = spec.substr(b + 1);
  BoardInfo info;
  if (board.empty() || board.size() > BoardInfo::kMaxName || rev.empty() ||
      rev.size() > BoardInfo::kMaxRevision || serial.empty() ||
      serial.size() > BoardInfo::kMaxSerial) {
    return false;
  }
  std::memcpy(info.board, board.data(), board.size());
  std::memcpy(info.revision, rev.data(), rev.size());
  std::memcpy(info.serial, serial.data(), serial.size());
  return save_board_info(store, info);
}

}  // namespace

int main(int argc, char** argv) {
  StdoutOut out;
  SerialDatagram net(out);
  RebuildableLink link;
  sim::Node node(sim::tl_sn_001_board(), 64 * 1024, &link);
  node.poll_link = true;
  std::string board_id = "TL-SN-001:A:SIM-0001";
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
    } else if (arg.rfind("--provision=", 0) == 0 &&
               !provision_from_arg(node.store, arg.substr(12))) {
      std::fprintf(stderr, "bad %s (want <device_id>:<16-32 byte hex key>)\n", arg.c_str());
      return 2;
    } else if (arg.rfind("--board-id=", 0) == 0) {
      board_id = arg.substr(11);
    } else if (arg == "--no-board-id") {
      board_id.clear();
    }
  }
  if (!board_id.empty() && !board_id_from_arg(node.store, board_id)) {
    std::fprintf(stderr, "bad --board-id=%s (want <board>:<revision>:<serial>)\n",
                 board_id.c_str());
    return 2;
  }
  for (int i = 1; i < argc; ++i) {  // faults after the cards exist
    const std::string arg = argv[i];
    if (arg.rfind("--fault=", 0) == 0 && !sim::apply_fault(node, arg.substr(8))) {
      std::fprintf(stderr, "unknown fault %s\n", arg.c_str());
      return 2;
    }
  }
  auto boot = [&] {
    link.link = std::make_unique<ProvisionedLink>(net, node.store);
    node.boot();
  };
  boot();

  std::string pending;
  char buf[4096];
  auto last = std::chrono::steady_clock::now();
  bool eof = false;
  while (!eof) {
    pollfd p{0, POLLIN, 0};
    if (::poll(&p, 1, 10) > 0) {
      const ssize_t n = ::read(0, buf, sizeof buf);
      if (n <= 0) {
        eof = true;
      } else {
        pending.append(buf, static_cast<std::size_t>(n));
      }
    }
    std::size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, nl);
      pending.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty() || net.push_line(line.data(), line.size())) continue;
      if (line.rfind("sim ", 0) == 0) {
        const std::string rest = line.substr(4);
        if (rest.rfind("fault ", 0) == 0) {
          const bool ok = sim::apply_fault(node, rest.substr(6));
          sim_done(ok, ok ? nullptr : "unknown fault");
        } else if (rest.rfind("run ", 0) == 0) {
          node.run_for(std::stoull(rest.substr(4)));
          sim_done(true);
        } else if (rest == "boot") {
          boot();
          sim_done(true);
        } else if (rest == "deepsleep") {
          // The ext1 mask: slot WAKE lines and EXP_WAKE, as armed by the runtime.
          std::uint32_t lines = 0;
          auto consider = [&](std::uint8_t l) {
            if (l < 32 && node.gpio.read(l) && node.sleep.wake_enabled(l)) lines |= 1u << l;
          };
          for (const auto& w : node.board.slots) consider(w.wake_line);
          consider(node.board.expander_wake_line);
          if (lines != 0) {
            link.link = std::make_unique<ProvisionedLink>(net, node.store);
            node.wake_from_deep_sleep(lines);
          }
          sim_done(true);
        } else {
          sim_done(false, "unknown sim command");
        }
        continue;
      }
      Diag diag(node.platform(), node.runtime(), node.has_board_io() ? &node.board_io() : nullptr);
      diag.handle_line(line.c_str(), out);
      if (diag.reboot_requested()) boot();
    }
    // Virtual time follows wall time (at most 100 ms per loop, so a stall cannot jump ahead).
    const auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
    if (ms > 0) {
      node.run_for(static_cast<talos::fw::TimeMs>(ms > 100 ? 100 : ms));
      last = now;
    }
  }
  return 0;
}
