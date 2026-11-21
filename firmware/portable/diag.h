// Bench diagnostic console (bench-test-plan.md §3, §7). One text command per
// line in; one or more machine-readable lines out, each "@T {json}", ending
// with "@T {"cmd":...,"done":true,"ok":true|false[,"err":"..."]}". Log lines
// never start with "@T", so a host can mix them on one serial port.
//
// Heap-free: fixed buffers, no exceptions. Runs on the ESP32 over
// USB-serial-JTAG and on the virtual board (firmware/sim:bench_node).
//
// Commands (one per line):
//   help, id, slots, power, rescan
//   i2c scan [root|<slot>]
//   i2c rd <addr> <reg> <n> [slot=<n>]
//   i2c wr <addr> <reg> <byte>... [slot=<n>]
//   mux [<mask>]
//   exp <addr>
//   rail <slot> aon|sw on|off
//   uart <slot>|off
//   selftest i2c
//   selftest slot|imu|sht <slot>
//   provision <device_id> <key hex, 16-32 bytes> | identity | reboot
//   board-id [<board> <revision> <serial>]   write-once board identity (D-09)
//   led | led service on|off                 indicator state; slot LEDs on battery (L-03)
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "firmware/portable/board_io.h"
#include "firmware/portable/hal.h"
#include "firmware/portable/runtime.h"

namespace talos::fw {

// Board identity in the permanent partition (StoreKey::kBoardInfo, D-09).
std::optional<BoardInfo> load_board_info(IStore& store);
bool save_board_info(IStore& store, const BoardInfo& info);

class IDiagOut {
 public:
  virtual ~IDiagOut() = default;
  virtual void write(const char* s, std::size_t n) = 0;
};

// Builds one "@T {...}\n" line in a fixed buffer.
class JsonLine {
 public:
  JsonLine();
  JsonLine& str(const char* key, const char* value);
  JsonLine& num(const char* key, std::int64_t value);
  JsonLine& hex(const char* key, std::uint32_t value);  // "0x1234" string, for codes and addresses
  JsonLine& boolean(const char* key, bool value);
  JsonLine& nums(const char* key, const std::int32_t* values, std::size_t n);
  void emit(IDiagOut& out);
  bool overflowed() const { return overflow_; }

 private:
  void raw(const char* s);
  void key(const char* k);
  char buf_[384];
  std::size_t n_ = 0;
  bool first_ = true;
  bool overflow_ = false;
};

class Diag {
 public:
  static constexpr std::size_t kMaxLine = 160;
  // `board_io` may be null on boards without expanders.
  Diag(Platform& p, NodeRuntime& runtime, BoardIo* board_io) : p_(p), rt_(runtime), io_(board_io) {}
  void handle_line(const char* line, IDiagOut& out);
  // Set by `reboot` (and after `provision`): the platform restarts the MCU.
  bool reboot_requested() const { return reboot_; }

 private:
  struct Args {
    const char* v[16];
    int n = 0;
  };
  bool cmd_id(IDiagOut& out);
  bool cmd_slots(IDiagOut& out);
  bool cmd_power(IDiagOut& out, const char*& err);
  bool cmd_i2c(const Args& a, IDiagOut& out, const char*& err);
  bool cmd_mux(const Args& a, IDiagOut& out, const char*& err);
  bool cmd_exp(const Args& a, IDiagOut& out, const char*& err);
  bool cmd_rail(const Args& a, IDiagOut& out, const char*& err);
  bool cmd_uart(const Args& a, IDiagOut& out, const char*& err);
  bool cmd_provision(const Args& a, IDiagOut& out, const char*& err);
  bool cmd_identity(IDiagOut& out);
  bool cmd_board_id(const Args& a, IDiagOut& out, const char*& err);
  bool cmd_led(const Args& a, IDiagOut& out, const char*& err);
  bool selftest_i2c(IDiagOut& out);
  bool selftest_slot(std::uint8_t slot, IDiagOut& out, const char*& err);
  bool selftest_imu(std::uint8_t slot, IDiagOut& out, const char*& err);
  bool selftest_sht(std::uint8_t slot, IDiagOut& out, const char*& err);

  bool select(std::uint8_t slot);
  void deselect();
  bool probe(std::uint8_t addr);
  bool check(IDiagOut& out, const char* test, bool ok);  // emits {"t":test,"ok":ok}

  Platform& p_;
  NodeRuntime& rt_;
  BoardIo* io_;
  bool reboot_ = false;
};

}  // namespace talos::fw
