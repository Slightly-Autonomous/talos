// Datagrams over the console: framing, identity and the provisioned link.
#include "firmware/portable/serial_link.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "firmware/sim/virtual_board.h"

namespace talos::fw {
namespace {

class Lines final : public ILineSink {
 public:
  void write(const char* s, std::size_t n) override { text.append(s, n); }
  std::string text;
};

std::string b64(const std::string& in) {
  char out[64];
  const auto n =
      base64::encode(reinterpret_cast<const std::uint8_t*>(in.data()), in.size(), out, sizeof out);
  return std::string(out, n);
}

TEST(Base64, Rfc4648Vectors) {
  EXPECT_EQ(b64(""), "");
  EXPECT_EQ(b64("f"), "Zg==");
  EXPECT_EQ(b64("fo"), "Zm8=");
  EXPECT_EQ(b64("foo"), "Zm9v");
  EXPECT_EQ(b64("foob"), "Zm9vYg==");
  EXPECT_EQ(b64("fooba"), "Zm9vYmE=");
  EXPECT_EQ(b64("foobar"), "Zm9vYmFy");
  std::uint8_t out[8];
  EXPECT_EQ(base64::decode("Zm9vYmE=", 8, out, sizeof out), std::optional<std::size_t>(5));
  EXPECT_EQ(std::memcmp(out, "fooba", 5), 0);
  EXPECT_FALSE(base64::decode("Zm9", 3, out, sizeof out));       // not a multiple of 4
  EXPECT_FALSE(base64::decode("Zm=v", 4, out, sizeof out));      // padding in the middle
  EXPECT_FALSE(base64::decode("Zm9v!!==", 8, out, sizeof out));  // bad characters
}

TEST(SerialDatagram, FramesRoundTripAndLeaveOtherLinesAlone) {
  Lines a_out, b_out;
  SerialDatagram a(a_out), b(b_out);
  std::vector<std::uint8_t> data(1000);
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 7);
  ASSERT_TRUE(a.send(data.data(), data.size()));
  ASSERT_EQ(a_out.text.rfind("@F ", 0), 0u);
  ASSERT_EQ(a_out.text.back(), '\n');
  EXPECT_TRUE(b.push_line(a_out.text.data(), a_out.text.size()));
  std::vector<std::uint8_t> got(SerialDatagram::kMaxDatagram);
  ASSERT_EQ(b.receive(got.data(), got.size()), data.size());
  EXPECT_TRUE(std::equal(data.begin(), data.end(), got.begin()));
  EXPECT_EQ(b.receive(got.data(), got.size()), 0u);
  // Diag replies and logs are not frames.
  EXPECT_FALSE(b.push_line("@T {\"t\":\"id\"}", 13));
  EXPECT_FALSE(b.push_line("I (123) talos: boot", 19));
  // A garbled frame is consumed and counted, never delivered.
  EXPECT_TRUE(b.push_line("@F !!!!", 7));
  EXPECT_EQ(b.dropped(), 1u);
  EXPECT_EQ(b.receive(got.data(), got.size()), 0u);
}

TEST(SerialDatagram, FullInboxDropsAndCounts) {
  Lines out;
  SerialDatagram d(out);
  for (std::size_t i = 0; i < SerialDatagram::kInbox + 2; ++i)
    EXPECT_TRUE(d.push_line("@F AAAA", 7));
  EXPECT_EQ(d.dropped(), 2u);
}

TEST(Identity, RoundTripsAndRejectsShortKeys) {
  sim::Store store;
  EXPECT_FALSE(load_identity(store));
  DeviceIdentity id;
  std::strcpy(id.device_id, "node-12");
  id.key_len = 8;  // too short to be a key
  EXPECT_FALSE(save_identity(store, id));
  id.key_len = 16;
  for (int i = 0; i < 16; ++i) id.key[i] = static_cast<std::uint8_t>(i + 1);
  ASSERT_TRUE(save_identity(store, id));
  const auto back = load_identity(store);
  ASSERT_TRUE(back);
  EXPECT_STREQ(back->device_id, "node-12");
  EXPECT_EQ(back->key_len, 16);
}

TEST(ProvisionedLink, UnprovisionedNodeKeepsEventsQueued) {
  Lines out;
  SerialDatagram net(out);
  sim::Store store;
  ProvisionedLink link(net, store);
  EXPECT_FALSE(link.provisioned());
  DeviceEvent e;
  EXPECT_FALSE(link.send(e));  // the runtime's queue keeps it
  EXPECT_TRUE(out.text.empty());
}

}  // namespace
}  // namespace talos::fw
