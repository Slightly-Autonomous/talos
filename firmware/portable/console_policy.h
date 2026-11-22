// USB console lock (ADR 0019 amendment 2026-11-22; ADR 0013 amendment 2026-11-22).
//
// TL-SN-002's USB-C port stays reachable from outside the closed enclosure (owner,
// 2026-11-22). With BoardSupport::console_lock = kLidClosed the diag console is
// READ-ONLY while the debounced tamper loop reads closed: it still answers what
// the node is and how it is (id, slots, power, identity, led, ...), and refuses
// every command that changes state or could blind the node. Opening the lid (the
// P0 tamper alarm) unlocks it.
//
// Pure and header-only: no I/O, no heap, no clock. Diag and the twin's L1 model
// both classify through it, so they cannot disagree.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace talos::fw {

// The error a locked console returns (the done line's "err").
inline constexpr const char* kConsoleLockedError = "locked: open the enclosure";
// Refusals are reported to the hub (EventKind::kConsoleLocked) at most this often;
// the first one at once, later ones counted and sent when the window ends.
inline constexpr std::uint32_t kConsoleReportMs = 60'000;

enum class ConsoleAccess : std::uint8_t {
  kReadOnly = 0,  // reports state, changes nothing the node relies on
  kMutating = 1,  // refused while locked (also anything unknown: fail closed)
};

// argv[0] is the command. Read-only:
//   help, id, slots, power, identity     what the node is and how it is
//   i2c scan [...]                        address probes only
//   exp <addr>                            expander registers, read
//   selftest i2c                          root-bus probes and register read-back
//   mux | board-id | led                  without arguments: a readout
// Everything else is mutating, including:
//   provision, board-id <...>, i2c wr, rail, uart, mux <mask>, rescan, reboot,
//   led service on|off, selftest slot|imu|sht (they write sensor registers and the
//   heater, then re-initialise the driver), and unknown commands.
//   `i2c rd` is mutating too: reading a latched source register (LIS2DW12
//   WAKE_UP_SRC/ALL_INT_SRC, TMAG5273 conversion status) clears it, which could
//   swallow the event the node is about to report.
inline ConsoleAccess console_access(const char* const* argv, int argc) {
  if (argc <= 0 || argv == nullptr || argv[0] == nullptr) return ConsoleAccess::kReadOnly;
  auto is = [&](const char* name) { return std::strcmp(argv[0], name) == 0; };
  if (is("help") || is("id") || is("slots") || is("power") || is("identity") || is("exp")) {
    return ConsoleAccess::kReadOnly;
  }
  if (is("mux") || is("board-id") || is("led")) {
    return argc == 1 ? ConsoleAccess::kReadOnly : ConsoleAccess::kMutating;
  }
  if (is("i2c")) {
    return argc >= 2 && std::strcmp(argv[1], "scan") == 0 ? ConsoleAccess::kReadOnly
                                                          : ConsoleAccess::kMutating;
  }
  if (is("selftest")) {
    return argc == 2 && std::strcmp(argv[1], "i2c") == 0 ? ConsoleAccess::kReadOnly
                                                         : ConsoleAccess::kMutating;
  }
  return ConsoleAccess::kMutating;
}

// The same for a whole console line (tokenised on spaces and tabs, at most 16 words).
inline ConsoleAccess console_line_access(const char* line) {
  char buf[160];
  std::size_t len = line ? std::strlen(line) : 0;
  if (len >= sizeof buf) len = sizeof buf - 1;
  if (len) std::memcpy(buf, line, len);
  buf[len] = 0;
  const char* argv[16];
  int argc = 0;
  char* p = buf;
  while (*p && argc < 16) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') *p++ = 0;
    if (!*p) break;
    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
  }
  return console_access(argv, argc);
}

}  // namespace talos::fw
