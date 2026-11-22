#include "firmware/portable/wire.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/sha256/sha256.h"

namespace talos::fw::wire {
namespace {

const std::string kSecret = "node-12-device-secret";
const Key kKey{reinterpret_cast<const std::uint8_t*>(kSecret.data()), kSecret.size()};

TEST(Hmac, Rfc4231Case2) {
  const std::string key = "Jefe", data = "what do ya want for nothing?";
  HmacSha256 h(reinterpret_cast<const std::uint8_t*>(key.data()), key.size());
  h.update(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
  const auto d = h.finish();
  static constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  for (auto b : d) {
    hex += kHex[b >> 4];
    hex += kHex[b & 0xF];
  }
  EXPECT_EQ(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

DeviceEvent sample(std::uint32_t seq) {
  DeviceEvent e;
  e.boot_epoch = 7;
  e.seq = seq;
  e.slot = 0;
  e.kind = EventKind::kContact;
  e.priority = Priority::kP0Critical;
  e.at_ms = 123456789;
  e.flag = true;
  e.a = -5;
  e.b = 42;
  return e;
}

TEST(Wire, UplinkRoundTripAndTamperDetection) {
  DeviceEvent events[2] = {sample(1), sample(2)};
  std::uint8_t buf[kMaxFrame];
  const auto n = encode_uplink(events, 2, kKey, buf, sizeof buf);
  ASSERT_EQ(n, 2 + 2 * kEventLen + kTagLen);
  std::vector<DeviceEvent> got;
  Tag tag{};
  ASSERT_EQ(decode_uplink(buf, n, kKey, &tag, [&](const DeviceEvent& e) { got.push_back(e); }),
            DecodeError::kNone);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[1], events[1]);

  buf[5] ^= 0x01;  // flip a bit in the payload
  EXPECT_EQ(decode_uplink(buf, n, kKey, nullptr, [](const DeviceEvent&) {}), DecodeError::kBadTag);
  buf[5] ^= 0x01;
  const std::string other = "someone-else";
  const Key wrong{reinterpret_cast<const std::uint8_t*>(other.data()), other.size()};
  EXPECT_EQ(decode_uplink(buf, n, wrong, nullptr, [](const DeviceEvent&) {}), DecodeError::kBadTag);
  EXPECT_EQ(decode_uplink(buf, n - 1, kKey, nullptr, [](const DeviceEvent&) {}),
            DecodeError::kBadLength);
}

TEST(Wire, TamperKindRoundTripsAndUnknownKindsAreRefused) {
  DeviceEvent e = sample(3);
  e.slot = kNoSlot;
  e.kind = EventKind::kTamper;  // appended after kPowerMode: older kinds keep their numbers
  EXPECT_EQ(static_cast<int>(EventKind::kTamper), 10);
  std::uint8_t buf[kMaxFrame];
  auto n = encode_uplink(&e, 1, kKey, buf, sizeof buf);
  std::vector<DeviceEvent> got;
  ASSERT_EQ(decode_uplink(buf, n, kKey, nullptr, [&](const DeviceEvent& d) { got.push_back(d); }),
            DecodeError::kNone);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0], e);
  // TL-SN-002's USER button (ADR 0019): appended after kTamper.
  e.kind = EventKind::kUserButton;
  e.a = static_cast<std::int32_t>(UserButtonAction::kCommissionRequest);
  EXPECT_EQ(static_cast<int>(EventKind::kUserButton), 11);
  n = encode_uplink(&e, 1, kKey, buf, sizeof buf);
  got.clear();
  ASSERT_EQ(decode_uplink(buf, n, kKey, nullptr, [&](const DeviceEvent& d) { got.push_back(d); }),
            DecodeError::kNone);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0], e);
  // TL-SN-002's console-lock refusals (ADR 0019 amendment 2026-11-22): appended.
  e.kind = EventKind::kConsoleLocked;
  e.a = 3;
  e.b = 7;
  EXPECT_EQ(static_cast<int>(EventKind::kConsoleLocked), 12);
  n = encode_uplink(&e, 1, kKey, buf, sizeof buf);
  got.clear();
  ASSERT_EQ(decode_uplink(buf, n, kKey, nullptr, [&](const DeviceEvent& d) { got.push_back(d); }),
            DecodeError::kNone);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0], e);
  // The reset report (2026-11-22): appended as 13.
  e.kind = EventKind::kReset;
  EXPECT_EQ(static_cast<int>(EventKind::kReset), 13);
  n = encode_uplink(&e, 1, kKey, buf, sizeof buf);
  EXPECT_EQ(decode_uplink(buf, n, kKey, nullptr, [](const DeviceEvent&) {}), DecodeError::kNone);
  e.kind = static_cast<EventKind>(14);
  n = encode_uplink(&e, 1, kKey, buf, sizeof buf);
  EXPECT_NE(decode_uplink(buf, n, kKey, nullptr, [](const DeviceEvent&) {}), DecodeError::kNone);
}

TEST(Wire, DownlinkIsBoundToItsRequest) {
  Tag req1{}, req2{};
  req1[0] = 1;
  req2[0] = 2;
  std::uint8_t buf[kMaxFrame];
  DownlinkWriter w(buf, sizeof buf);
  ASSERT_TRUE(w.add(Ack{7, 1}));
  ASSERT_TRUE(w.add(ConfigUpdate{3, 30'000, 60'000, 400}));
  OtaChunk c;
  c.offset = 256;
  c.length = 4;
  c.data = {1, 2, 3, 4};
  ASSERT_TRUE(w.add(c));
  ASSERT_TRUE(w.add(RebootRequest{}));
  const auto n = w.finish(kKey, req1);
  std::vector<Downlink> items;
  ASSERT_EQ(decode_downlink(buf, n, kKey, req1, [&](const Downlink& d) { items.push_back(d); }),
            DecodeError::kNone);
  ASSERT_EQ(items.size(), 4u);
  EXPECT_EQ(std::get<ConfigUpdate>(items[1]).impact_threshold_mg, 400u);
  EXPECT_EQ(std::get<OtaChunk>(items[2]).data[3], 4);
  // Replaying this reply against another request fails authentication.
  EXPECT_EQ(decode_downlink(buf, n, kKey, req2, [](const Downlink&) {}), DecodeError::kBadTag);
}

TEST(Wire, ConfigWithAPowerProfileUsesTheConfig2Item) {
  const Tag req{};
  std::uint8_t buf[kMaxFrame];
  DownlinkWriter w(buf, sizeof buf);
  ConfigUpdate keep{3, 30'000, 60'000, 400};
  ConfigUpdate prof = keep;
  prof.revision = 4;
  prof.power_profile = static_cast<std::uint8_t>(PowerProfile::kSleepyChild);
  ASSERT_TRUE(w.add(keep));
  ASSERT_TRUE(w.add(prof));
  const auto n = w.finish(kKey, req);
  EXPECT_EQ(buf[2], 2);       // first item: plain kConfig (older firmware decodes it)
  EXPECT_EQ(buf[2 + 17], 6);  // second: kConfig2
  std::vector<Downlink> items;
  ASSERT_EQ(decode_downlink(buf, n, kKey, req, [&](const Downlink& d) { items.push_back(d); }),
            DecodeError::kNone);
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(std::get<ConfigUpdate>(items[0]).power_profile, ConfigUpdate::kKeepProfile);
  EXPECT_EQ(std::get<ConfigUpdate>(items[1]).power_profile,
            static_cast<std::uint8_t>(PowerProfile::kSleepyChild));
  EXPECT_EQ(std::get<ConfigUpdate>(items[1]).impact_threshold_mg, 400u);
}

TEST(Wire, RandomFramesNeverDecodeOrCrash) {
  std::uint64_t s = 99;
  int accepted = 0;
  for (int i = 0; i < 20000; ++i) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(s >> 55));
    for (auto& b : buf) {
      s = s * 6364136223846793005ull + 1442695040888963407ull;
      b = static_cast<std::uint8_t>(s >> 56);
    }
    if (!buf.empty()) buf[0] = kVersion;
    Tag t{};
    accepted += decode_uplink(buf.data(), buf.size(), kKey, nullptr, [](const DeviceEvent&) {}) ==
                DecodeError::kNone;
    accepted += decode_downlink(buf.data(), buf.size(), kKey, t, [](const Downlink&) {}) ==
                DecodeError::kNone;
  }
  EXPECT_EQ(accepted, 0);  // forging a 64-bit tag by chance: not in this lifetime
}

}  // namespace
}  // namespace talos::fw::wire
