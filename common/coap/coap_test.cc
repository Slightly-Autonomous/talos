#include "common/coap/coap.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace talos::coap {
namespace {

TEST(Coap, RoundTripsConPostWithPathTokenPayload) {
  Message m;
  m.type = Type::kCon;
  m.code = kPost;
  m.message_id = 0xBEEF;
  m.token_len = 4;
  m.token = {1, 2, 3, 4};
  ASSERT_TRUE(m.add_segment("d"));
  ASSERT_TRUE(m.add_segment("a-rather-long-device-id"));  // > 12 bytes: extended length
  ASSERT_TRUE(m.add_segment("e"));
  const std::uint8_t body[] = {9, 8, 7};
  m.payload = body;
  m.payload_len = 3;
  std::uint8_t buf[128];
  const auto n = build(m, buf, sizeof buf);
  ASSERT_GT(n, 0u);
  Message p;
  ASSERT_TRUE(parse(buf, n, p));
  EXPECT_EQ(p.type, Type::kCon);
  EXPECT_EQ(p.code, kPost);
  EXPECT_EQ(p.message_id, 0xBEEF);
  EXPECT_EQ(p.token_len, 4);
  ASSERT_EQ(p.path_count, 3u);
  EXPECT_EQ(p.segment(1), "a-rather-long-device-id");
  ASSERT_EQ(p.payload_len, 3u);
  EXPECT_EQ(p.payload[2], 7);
}

TEST(Coap, RejectsMalformedInput) {
  Message p;
  const std::uint8_t short_hdr[] = {0x40, 0x02};
  EXPECT_FALSE(parse(short_hdr, 2, p));
  const std::uint8_t bad_version[] = {0x80, 0x02, 0, 1};
  EXPECT_FALSE(parse(bad_version, 4, p));
  const std::uint8_t bad_tkl[] = {0x49, 0x02, 0, 1};  // token length 9
  EXPECT_FALSE(parse(bad_tkl, 4, p));
  const std::uint8_t marker_no_payload[] = {0x40, 0x02, 0, 1, 0xFF};
  EXPECT_FALSE(parse(marker_no_payload, 5, p));
  const std::uint8_t option_overrun[] = {0x40, 0x02, 0, 1, 0xB5, 'a'};  // says 5, has 1
  EXPECT_FALSE(parse(option_overrun, 6, p));
}

TEST(Coap, RandomBytesNeverCrashOrOverread) {
  std::uint64_t s = 0x1234;
  for (int i = 0; i < 20000; ++i) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(s >> 58));
    for (auto& b : buf) {
      s = s * 6364136223846793005ull + 1442695040888963407ull;
      b = static_cast<std::uint8_t>(s >> 56);
    }
    Message p;
    (void)parse(buf.data(), buf.size(), p);  // ASan/UBSan catch overreads
  }
  SUCCEED();
}

}  // namespace
}  // namespace talos::coap
