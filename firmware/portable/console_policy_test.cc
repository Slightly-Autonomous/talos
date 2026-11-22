// The USB console lock's command classification (console_policy.h): what a closed
// TL-SN-002 still answers, and that everything else (unknown included) is refused.
#include "firmware/portable/console_policy.h"

#include <gtest/gtest.h>

namespace talos::fw {
namespace {

bool read_only(const char* line) { return console_line_access(line) == ConsoleAccess::kReadOnly; }

TEST(ConsolePolicy, ReadoutsStayOpen) {
  for (const char* l :
       {"help", "id", "slots", "power", "identity", "exp 0x20", "mux", "board-id", "led",
        "i2c scan", "i2c scan root", "i2c scan 2", "selftest i2c", "  id  \r\n", ""}) {
    EXPECT_TRUE(read_only(l)) << '"' << l << '"';
  }
}

TEST(ConsolePolicy, MutationsAndUnknownsAreRefused) {
  for (const char* l : {"provision node-1 00112233445566778899aabbccddeeff",
                        "board-id TL-SN-002 A SN-1",
                        "i2c wr 0x35 0x02 0x00",
                        "i2c rd 0x19 0x27 1",
                        "i2c",
                        "rail 2 aon off",
                        "uart 0",
                        "uart off",
                        "mux 1",
                        "rescan",
                        "reboot",
                        "led service on",
                        "led service off",
                        "selftest imu 1",
                        "selftest sht 3",
                        "selftest slot 0",
                        "selftest",
                        "selftest i2c extra",
                        "frobnicate",
                        "ID",
                        "help;reboot"}) {
    EXPECT_FALSE(read_only(l)) << '"' << l << '"';
  }
}

TEST(ConsolePolicy, ArgvFormMatchesTheLineForm) {
  const char* argv[] = {"led", "service", "on"};
  EXPECT_EQ(console_access(argv, 1), ConsoleAccess::kReadOnly);
  EXPECT_EQ(console_access(argv, 3), ConsoleAccess::kMutating);
  EXPECT_EQ(console_access(nullptr, 0), ConsoleAccess::kReadOnly);  // an empty line does nothing
}

}  // namespace
}  // namespace talos::fw
