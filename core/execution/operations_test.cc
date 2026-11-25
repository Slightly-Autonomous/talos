#include "core/execution/operations.h"

#include <gtest/gtest.h>

namespace talos {
namespace {

const CapabilityRef kLock{DeviceId("lock-3"), std::nullopt, CapabilityId("access.lock")};
const CapabilityRef kSpeaker{DeviceId("patio_speaker"), std::nullopt, CapabilityId("audio.speak")};
const CapabilityRef kNotifier{DeviceId("phone"), std::nullopt, CapabilityId("notify")};

Command command(std::string operation, CapabilityRef target, CommandArgs args) {
  Command c;
  c.id = CommandId("c");
  c.operation = std::move(operation);
  c.target = std::move(target);
  c.args = std::move(args);
  c.created_at = Timestamp{1'000};
  c.expires_at = Timestamp{6'000};
  return c;
}

TEST(Operations, CatalogIsTheSourceOfRiskArgsAndTarget) {
  const auto* lock = find_operation("access.set_locked");
  ASSERT_NE(lock, nullptr);
  EXPECT_EQ(lock->risk, RiskClass::kSecurityCritical);
  EXPECT_EQ(lock->target_capability, "access.lock");
  EXPECT_EQ(find_operation("door.open"), nullptr);
}

TEST(Operations, MismatchedArgsTargetOrOperationAreRefused) {
  EXPECT_EQ(normalize_command(command("audio.speak", kLock, SetLocked{false})).error().code,
            "args_do_not_match_operation");
  EXPECT_EQ(normalize_command(command("audio.speak", kLock, AudioSpeak{"x", 50})).error().code,
            "target_does_not_match_operation");
  EXPECT_EQ(normalize_command(command("access.set_locked", kLock, std::monostate{})).error().code,
            "args_do_not_match_operation");
  EXPECT_EQ(normalize_command(command("alarm.disarm", kLock, std::monostate{})).error().code,
            "unsupported_operation");
  EXPECT_EQ(normalize_command(command("", kLock, SetLocked{false})).error().code,
            "unsupported_operation");
}

TEST(Operations, ExpectationIsDerivedAndACallerExpectMustEqualIt) {
  auto unlock = command("access.set_locked", kLock, SetLocked{false});
  auto n = normalize_command(unlock);
  ASSERT_TRUE(n.ok());
  EXPECT_EQ(n.value().verification, VerificationMode::kObservedState);
  EXPECT_EQ(n.value().expect, (ExpectedObservation{kLock, LockState{false}}));

  unlock.expect = ExpectedObservation{kLock, LockState{false}};
  EXPECT_TRUE(normalize_command(unlock).ok());  // equal: accepted
  unlock.expect = ExpectedObservation{kLock, LockState{true}};
  EXPECT_EQ(normalize_command(unlock).error().code, "expectation_does_not_match_operation");
  unlock.expect = ExpectedObservation{
      CapabilityRef{kLock.device, std::nullopt, CapabilityId("device.health")}, Heartbeat{1}};
  EXPECT_EQ(normalize_command(unlock).error().code, "expectation_does_not_match_operation");

  auto speak = normalize_command(command("audio.speak", kSpeaker, AudioSpeak{"x", 50}));
  EXPECT_EQ(speak.value().expect, (ExpectedObservation{kSpeaker, AudioPlayback{true}}));
}

TEST(Operations, AckOnlyNeverCarriesAnExpectation) {
  auto notify = normalize_command(command("notify", kNotifier, Notify{"owner", "hi"}));
  ASSERT_TRUE(notify.ok());
  EXPECT_EQ(notify.value().verification, VerificationMode::kAckOnly);
  EXPECT_FALSE(notify.value().expect.has_value());

  auto speak = command("audio.speak", kSpeaker, AudioSpeak{"x", 50});
  speak.verification = VerificationMode::kAckOnly;
  speak.expect = ExpectedObservation{kSpeaker, AudioPlayback{true}};
  auto n = normalize_command(speak);
  ASSERT_TRUE(n.ok());
  EXPECT_EQ(n.value().verification, VerificationMode::kAckOnly);
  EXPECT_FALSE(n.value().expect.has_value());
}

TEST(Operations, TtlAndWindowAreBoundedByRisk) {
  auto lock = command("access.set_locked", kLock, SetLocked{true});
  lock.expires_at = lock.created_at + Millis{30'000};
  EXPECT_TRUE(normalize_command(lock).ok());
  lock.expires_at = lock.created_at + Millis{30'001};
  EXPECT_EQ(normalize_command(lock).error().code, "ttl_out_of_bounds");

  auto speak = command("audio.speak", kSpeaker, AudioSpeak{"x", 50});
  speak.expires_at = speak.created_at + Millis{300'000};
  EXPECT_TRUE(normalize_command(speak).ok());
  speak.verification_window = Millis{300'001};
  EXPECT_EQ(normalize_command(speak).error().code, "verification_window_out_of_bounds");
  speak.verification_window = Millis{-1};
  EXPECT_EQ(normalize_command(speak).error().code, "verification_window_out_of_bounds");
}

// Red team #8: free text in command args is bounded.
TEST(Operations, FreeTextArgsAreBounded) {
  EXPECT_TRUE(
      normalize_command(command("audio.speak", kSpeaker, AudioSpeak{std::string(280, 'a'), 50}))
          .ok());
  EXPECT_EQ(
      normalize_command(command("audio.speak", kSpeaker, AudioSpeak{std::string(281, 'a'), 50}))
          .error()
          .code,
      "args_out_of_bounds");
  EXPECT_EQ(normalize_command(command("audio.speak", kSpeaker, AudioSpeak{"x", 101})).error().code,
            "args_out_of_bounds");
}

// ADR 0020: a bookmark is verified by the recorder's clip for this command,
// whatever media id the recorder picks; its args are bounded.
TEST(Operations, BookmarkDefaultsBoundsAndClipExpectation) {
  const CapabilityRef rec{DeviceId("cam_patio_video"), std::nullopt,
                          CapabilityId("camera.recording")};
  auto c = command("camera.bookmark", rec, Bookmark{});
  c.verification = VerificationMode::kObservedState;
  c.verification_window = Millis{60'000};
  auto n = normalize_command(c);
  ASSERT_TRUE(n.ok()) << n.error().message;
  EXPECT_EQ(std::get<Bookmark>(n.value().args).duration_s, kDefaultBookmarkS);
  EXPECT_EQ(std::get<Bookmark>(n.value().args).label, "talos");
  EXPECT_EQ(n.value().verification, VerificationMode::kObservedState);
  ASSERT_TRUE(n.value().expect);
  EXPECT_EQ(std::get<ClipRecorded>(n.value().expect->payload).command_id, "c");
  EXPECT_EQ(find_operation("camera.bookmark")->risk, RiskClass::kBenignOutput);

  for (Bookmark bad : {Bookmark{4, ""}, Bookmark{121, ""}, Bookmark{30, "Front Door"},
                       Bookmark{30, std::string(33, 'a')}}) {
    EXPECT_EQ(normalize_command(command("camera.bookmark", rec, bad)).error().code,
              "args_out_of_bounds");
  }
  EXPECT_EQ(normalize_command(command("camera.bookmark", kSpeaker, Bookmark{})).error().code,
            "target_does_not_match_operation");

  const ObservationPayload want = ClipRecorded{"c", {}};
  EXPECT_TRUE(
      expectation_met(want, ClipRecorded{"c", MediaRef{"frigate", "e1", 1, 2, true, true}}));
  EXPECT_FALSE(
      expectation_met(want, ClipRecorded{"other", MediaRef{"frigate", "e1", 1, 2, true, true}}));
  EXPECT_FALSE(expectation_met(want, LockState{true}));
  EXPECT_TRUE(expectation_met(LockState{true}, LockState{true}));
}

}  // namespace
}  // namespace talos
