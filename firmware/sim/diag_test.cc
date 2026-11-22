// The bench console against the virtual TL-SN-001: every self-test passes on a
// healthy board and names the fault on a broken one.
#include "firmware/portable/diag.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "firmware/sim/sim_faults.h"
#include "firmware/sim/virtual_board.h"

namespace talos::fw::sim {
namespace {

class Capture final : public IDiagOut {
 public:
  void write(const char* s, std::size_t n) override { text.append(s, n); }
  std::vector<std::string> lines() const {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\n') {
        out.push_back(text.substr(start, i - start));
        start = i + 1;
      }
    }
    return out;
  }
  std::string text;
};

struct Bench {
  explicit Bench(std::initializer_list<std::pair<std::uint8_t, ModuleCode>> cards = {})
      : node(tl_sn_001_board()) {
    for (auto [slot, code] : cards) node.insert(slot, code);
  }
  void boot() {
    node.boot();
    node.run_for(20);
  }
  Capture run(const std::string& cmd) {
    Capture c;
    Diag(node.platform(), node.runtime(), &node.board_io()).handle_line(cmd.c_str(), c);
    return c;
  }
  Node node;
};

// The record for test `t` in the output, or "" if absent.
std::string record(const Capture& c, const std::string& t) {
  for (const auto& l : c.lines()) {
    if (l.find("\"t\":\"" + t + "\"") != std::string::npos) return l;
  }
  return "";
}
bool passed(const Capture& c, const std::string& t) {
  return record(c, t).find("\"ok\":true") != std::string::npos;
}
bool done_ok(const Capture& c) {
  const auto l = c.lines();
  return !l.empty() && l.back().find("\"done\":true,\"ok\":true") != std::string::npos;
}

TEST(Diag, EveryLineIsTaggedAndEndsWithADoneRecord) {
  Bench b;
  b.boot();
  auto c = b.run("id");
  for (const auto& l : c.lines()) EXPECT_EQ(l.rfind("@T {", 0), 0u) << l;
  EXPECT_TRUE(done_ok(c));
  auto bad = b.run("frobnicate");
  EXPECT_NE(bad.lines().back().find("\"ok\":false,\"err\":\"unknown command\""), std::string::npos);
}

TEST(Diag, SlotsReportCodePresenceAndRails) {
  Bench b({{0, ModuleCode::kTlMod004Env}});
  b.boot();
  auto c = b.run("slots");
  EXPECT_NE(c.text.find("\"slot\":0,\"code\":\"0x0302\",\"fault\":0,\"enabled\":true,\"present\":1,"
                        "\"aon\":true,\"sw\":false"),
            std::string::npos)
      << c.text;
  EXPECT_NE(
      c.text.find("\"slot\":1,\"code\":\"0x0000\",\"fault\":0,\"enabled\":false,\"present\":0"),
      std::string::npos);
}

TEST(Diag, RootScanAndI2cSelftestOnAHealthyBoard) {
  Bench b;
  b.boot();
  auto scan = b.run("i2c scan");
  EXPECT_NE(scan.text.find("\"found\":[32,33,34,106]"), std::string::npos) << scan.text;
  auto st = b.run("selftest i2c");
  EXPECT_TRUE(passed(st, "i2c.root_scan")) << st.text;
  EXPECT_TRUE(passed(st, "i2c.mux_readback"));
  EXPECT_TRUE(passed(st, "i2c.expander_outputs"));
  EXPECT_TRUE(passed(st, "i2c.charger_config"));
  EXPECT_TRUE(done_ok(st));
}

TEST(Diag, I2cSelftestNamesAMissingCharger) {
  Bench b;
  apply_fault(b.node, "charger_nack");
  b.boot();
  auto st = b.run("selftest i2c");
  EXPECT_FALSE(passed(st, "i2c.root_scan"));
  EXPECT_NE(
      record(st, "i2c.root_scan").find("\"found\":[32,33,34,112],\"want\":[32,33,34,106,112]"),
      std::string::npos)
      << st.text;
  EXPECT_FALSE(done_ok(st));
}

TEST(Diag, SlotLoopbackPassesWithATestCard) {
  Bench b({{2, ModuleCode::kTlTest001}});
  b.boot();
  auto st = b.run("selftest slot 2");
  for (const char* t : {"slot.present", "slot.branch_isolation", "slot.wake", "slot.alert",
                        "slot.cs_n", "slot.reset_n"}) {
    EXPECT_TRUE(passed(st, t)) << t << "\n" << st.text;
  }
  EXPECT_TRUE(done_ok(st));
}

TEST(Diag, SlotLoopbackIsolatesEachBrokenContact) {
  for (const auto& [fault, test] : std::vector<std::pair<std::string, std::string>>{
           {"card_wake_open2", "slot.wake"}, {"card_alert_open2", "slot.alert"}}) {
    Bench b({{2, ModuleCode::kTlTest001}});
    apply_fault(b.node, fault);
    b.boot();
    auto st = b.run("selftest slot 2");
    EXPECT_FALSE(passed(st, test)) << fault;
    EXPECT_TRUE(passed(st, "slot.cs_n")) << fault;  // the other contacts still pass
    EXPECT_FALSE(done_ok(st));
  }
}

TEST(Diag, ImuSelfTestPassesAndCatchesADeadSensingElement) {
  Bench good({{1, ModuleCode::kTlMod003Imu}});
  good.boot();
  auto st = good.run("selftest imu 1");
  EXPECT_TRUE(passed(st, "imu.gravity")) << st.text;
  EXPECT_TRUE(passed(st, "imu.self_test")) << st.text;
  EXPECT_TRUE(good.node.runtime().slot(1).enabled);  // driver restored
  EXPECT_EQ(good.node.imu(1).regs[Lis2dw12Driver::kCtrl3] & Lis2dw12Driver::kCtrl3Lir,
            Lis2dw12Driver::kCtrl3Lir);
  Bench bad({{1, ModuleCode::kTlMod003Imu}});
  apply_fault(bad.node, "imu_no_selftest1");
  bad.boot();
  EXPECT_FALSE(passed(bad.run("selftest imu 1"), "imu.self_test"));
}

TEST(Diag, ShtHeaterTestPassesAndCatchesADeadHeater) {
  Bench good({{3, ModuleCode::kTlMod004Env}});
  good.boot();
  auto st = good.run("selftest sht 3");
  EXPECT_TRUE(passed(st, "sht.serial")) << st.text;
  EXPECT_TRUE(passed(st, "sht.heater")) << st.text;
  Bench bad({{3, ModuleCode::kTlMod004Env}});
  apply_fault(bad.node, "sht_dead_heater3");
  bad.boot();
  EXPECT_FALSE(passed(bad.run("selftest sht 3"), "sht.heater"));
}

TEST(Diag, ShtSelfTestWaitsOutAMeasurementAlreadyInFlight) {
  // The runtime's driver can be mid-measurement when the bench command arrives; the SHT40
  // NACKs until it finishes, which must not read as a bus or CRC fault.
  Bench b({{3, ModuleCode::kTlMod004Env}});
  b.boot();
  Platform& p = b.node.platform();
  const auto mask = static_cast<std::uint8_t>(1u << p.board.slots[3].i2c_branch);
  ASSERT_TRUE(p.i2c.write(p.board.mux_address, &mask, 1));
  const std::uint8_t measure = Sht40Driver::kMeasureHighPrecision;
  ASSERT_TRUE(p.i2c.write(Sht40Driver::kAddr, &measure, 1));
  auto st = b.run("selftest sht 3");
  EXPECT_TRUE(passed(st, "sht.serial")) << st.text;
  EXPECT_TRUE(passed(st, "sht.heater")) << st.text;
}

TEST(Diag, RailCommandsFollowTheSequencingRules) {
  Bench b({{0, ModuleCode::kTlTest001}});
  b.boot();
  EXPECT_TRUE(done_ok(b.run("rail 0 sw on")));
  EXPECT_TRUE(b.node.rev_a().sw_on(0));
  EXPECT_TRUE(done_ok(b.run("rail 0 aon off")));
  EXPECT_FALSE(b.node.rev_a().sw_on(0));
  auto refused = b.run("rail 0 sw on");
  EXPECT_NE(refused.text.find("refused: AON is off"), std::string::npos);
  EXPECT_FALSE(done_ok(b.run("uart 0")));  // unpowered: refused
}

TEST(Diag, PowerReportsSourceAndCharger) {
  Bench b;
  b.boot();
  auto c = b.run("power");
  EXPECT_NE(c.text.find("\"source\":\"usb\""), std::string::npos) << c.text;
  EXPECT_TRUE(passed(c, "charger") ||
              record(c, "charger").find("\"config_ok\":true") != std::string::npos);
  apply_fault(b.node, "expb_nack");
  EXPECT_NE(b.run("power").text.find("\"source\":\"unknown\""), std::string::npos);
}

TEST(Diag, RawI2cReadOnASlotBranch) {
  Bench b({{1, ModuleCode::kTlMod004Env}});
  b.boot();
  auto c = b.run("i2c rd 0x50 0 4 slot=1");
  EXPECT_NE(c.text.find("\"data\":[3,2,1,1]"), std::string::npos) << c.text;  // 0x0302 rev 1 v1
  EXPECT_EQ(b.node.i2c.mask(), 0);                                            // left deselected
}

TEST(Diag, BoardIdIsWrittenOnceAndSurvivesAWorkingPartitionErase) {
  Bench b;
  b.boot();
  EXPECT_NE(b.run("id").text.find("\"board\":\"\",\"rev\":\"\",\"serial\":\"\""),
            std::string::npos);
  auto w = b.run("board-id TL-SN-001 A SN-A-0001");
  EXPECT_TRUE(done_ok(w)) << w.text;
  EXPECT_NE(record(w, "board_id").find("\"board\":\"TL-SN-001\",\"rev\":\"A\""), std::string::npos);
  EXPECT_TRUE(done_ok(b.run("board-id TL-SN-001 A SN-A-0001")));  // same values: fine
  auto again = b.run("board-id TL-SN-001 B SN-A-0001");
  EXPECT_FALSE(done_ok(again));
  EXPECT_NE(again.text.find("write-once"), std::string::npos);
  EXPECT_FALSE(done_ok(b.run("board-id TL-SN-001 A bad/serial")));
  b.node.store.erase_working_partition();  // talos_id is a different partition
  b.boot();
  EXPECT_NE(b.run("id").text.find("\"board\":\"TL-SN-001\",\"rev\":\"A\",\"serial\":\"SN-A-0001\""),
            std::string::npos);
}

// Red team: the write-once label must name the board this firmware runs, so a
// flash step that silently left another image (or the wrong --board) can't burn a
// wrong, permanent identity.
TEST(Diag, BoardIdMustNameThisFirmwaresBoard) {
  Bench b;
  b.boot();
  EXPECT_NE(record(b.run("id"), "id").find("\"target\":\"TL-SN-001\""), std::string::npos);
  auto w = b.run("board-id TL-SN-002 A SN2-A-0001");
  EXPECT_FALSE(done_ok(w)) << w.text;
  EXPECT_NE(w.text.find("another board"), std::string::npos);
  EXPECT_NE(record(w, "board_id").find("\"programmed\":false"), std::string::npos);
  EXPECT_TRUE(done_ok(b.run("board-id TL-SN-001 A SN-A-0001")));
}

TEST(Diag, LedServiceModeLightsSlotLedsOnBattery) {
  Bench b({{0, ModuleCode::kTlMod004Env}});
  apply_fault(b.node, "no_vin");
  b.boot();
  EXPECT_NE(record(b.run("led"), "led").find("\"slot_lit\":[0,0,0,0]"), std::string::npos);
  EXPECT_TRUE(done_ok(b.run("led service on")));
  b.node.run_for(5);
  auto on = b.run("led");
  EXPECT_NE(record(on, "led").find("\"service\":true"), std::string::npos) << on.text;
  EXPECT_NE(record(on, "led").find("\"slot_lit\":[1,0,0,0]"), std::string::npos) << on.text;
  EXPECT_TRUE(b.node.rev_a().slot_led(0));
  EXPECT_TRUE(done_ok(b.run("led service off")));
  b.node.run_for(5);
  EXPECT_FALSE(b.node.rev_a().slot_led(0));
  EXPECT_FALSE(done_ok(b.run("led blink")));
}

// The archived TL-SN-001 has its USB-C inside the enclosure: ConsoleLock::kNever, so
// its console stays fully open with the lid closed (bench tooling depends on it).
TEST(Diag, Tl001ConsoleNeverLocks) {
  Bench b({{0, ModuleCode::kTlMod004Env}});
  b.boot();
  b.node.run_for(100);  // past the tamper debounce: the lid reads closed
  ASSERT_EQ(b.node.runtime().tamper_open(), std::optional<bool>{false});
  EXPECT_FALSE(b.node.runtime().console_locked());
  EXPECT_NE(record(b.run("id"), "id").find("\"console_locked\":false"), std::string::npos);
  EXPECT_TRUE(done_ok(b.run("led service on")));
  EXPECT_TRUE(done_ok(b.run("rail 0 aon on")));
  EXPECT_EQ(b.node.runtime().console_refusals(), 0u);
}

}  // namespace
}  // namespace talos::fw::sim
