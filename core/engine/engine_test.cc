#include "core/engine/engine.h"

#include <gtest/gtest.h>

#include "core/engine/preview.h"
#include "core/testing/fixtures.h"

namespace talos {
namespace {

const CapabilityRef kSpeaker{DeviceId("patio_speaker"), std::nullopt, CapabilityId("audio.speak")};
const CapabilityRef kLock{DeviceId("lock-3"), std::nullopt, CapabilityId("access.lock")};
const PrincipalId kOwner("user-33");
const PrincipalId kAgent("agent-1");

Rule backyard_rule() {
  Rule r;
  r.id = RuleId("backyard_motion_warning");
  r.property = PropertyId("house-01");
  r.principal = PrincipalId("rule:backyard");
  r.trigger = {"motion.detected", ZoneId("backyard")};
  r.conditions.armed_modes = {ArmMode::kArmedAway};
  r.conditions.schedule = TimeWindow{22 * 60, 6 * 60};
  RuleAction speak;
  speak.operation = "audio.speak";
  speak.target = kSpeaker;
  speak.args = AudioSpeak{"This property is monitored.", 65};
  speak.verification_window = Millis{3'000};
  speak.expect = ExpectedObservation{kSpeaker, AudioPlayback{true}};
  r.actions.push_back(speak);
  return r;
}

EngineConfig config() {
  EngineConfig c;
  c.property = testing::sample_property();
  c.rules = {backyard_rule()};
  const PropertyId house("house-01");
  c.principals = {
      {PrincipalId("rule:backyard"), PrincipalKind::kRule, {{house, RiskClass::kDisruptiveOutput}}},
      {kOwner, PrincipalKind::kHuman, {{house, RiskClass::kSecurityCritical}}},
      {kAgent, PrincipalKind::kAgent, {{house, RiskClass::kSecurityCritical}}},
  };
  c.freshness_max_age = Millis{60'000};
  c.local_minute_of_day = [](Timestamp) -> std::uint16_t { return 2 * 60; };  // 02:00
  return c;
}

ObservationEnvelope observation(const CapabilityRef& ref, std::uint64_t seq,
                                ObservationPayload payload, std::string type) {
  ObservationEnvelope e;
  e.property = PropertyId("house-01");
  e.source = ref;
  e.event_type = std::move(type);
  e.event_id = EventId(ref.device.str() + ":1:" + std::to_string(seq));
  e.position = {1, seq};
  e.payload = std::move(payload);
  return e;
}

Command lock_request(PrincipalId who, Timestamp now) {
  Command c;
  c.id = CommandId("lock-" + who.str());
  c.idempotency_key = c.id.str();
  c.principal = who;
  c.property = PropertyId("house-01");
  c.target = kLock;
  c.operation = "access.set_locked";
  c.args = SetLocked{true};
  c.created_at = now;
  c.expires_at = now + Millis{5'000};
  c.verification_window = Millis{5'000};
  c.expect = ExpectedObservation{kLock, LockState{true}};
  return c;
}

class EngineTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(engine.set_arm_mode(ArmMode::kArmedAway, kOwner)); }
  InMemoryJournal journal;
  VirtualClock clock{Timestamp{1'000}};
  HubEngine engine{config(), journal, clock};
};

TEST_F(EngineTest, BackyardIntrusionSpeaksAndVerifies) {
  engine.on_observation(testing::motion("node-07", 1, 1));
  auto out = engine.take_outbound();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].operation, "audio.speak");
  EXPECT_EQ(out[0].causation, EventId("node-07:1:1"));

  clock.advance(Millis{400});
  engine.on_observation(observation(kSpeaker, 1, AudioPlayback{true}, "audio.playback_started"));
  EXPECT_EQ(engine.state().commands.find(out[0].id)->status, CommandStatus::kVerified);
  EXPECT_TRUE(engine.take_outbound().empty());
}

TEST_F(EngineTest, DuplicateMotionDeliveryCausesOneDispatch) {
  engine.on_observation(testing::motion("node-07", 1, 1));
  engine.on_observation(testing::motion("node-07", 1, 1));
  EXPECT_EQ(engine.take_outbound().size(), 1u);
  EXPECT_EQ(engine.state().commands.records().size(), 1u);
}

TEST_F(EngineTest, SpeakerSilenceEndsUnknownNotSuccess) {
  engine.on_observation(testing::motion("node-07", 1, 1));
  auto id = engine.take_outbound().at(0).id;
  clock.advance(Millis{3'001});
  engine.tick();
  EXPECT_EQ(outcome_of(engine.state().commands.find(id)->status), CommandOutcome::kUnknown);
}

TEST_F(EngineTest, NothingHappensWhenDisarmed) {
  ASSERT_TRUE(engine.set_arm_mode(ArmMode::kDisarmed, kOwner));
  engine.on_observation(testing::motion("node-07", 1, 1));
  EXPECT_TRUE(engine.take_outbound().empty());
}

TEST_F(EngineTest, AgentCannotDisarm) {
  Status s = engine.set_arm_mode(ArmMode::kDisarmed, kAgent);
  ASSERT_FALSE(s);
  EXPECT_EQ(s.error().message, "requires_human_authorization");
  EXPECT_EQ(engine.state().arm_mode, ArmMode::kArmedAway);
}

TEST_F(EngineTest, LateSupersededObservationDoesNotTriggerRules) {
  engine.on_observation(testing::motion("node-07", 1, 2, /*detected=*/false));
  engine.on_observation(testing::motion("node-07", 1, 1, /*detected=*/true));  // reordered
  EXPECT_TRUE(engine.take_outbound().empty());
  EXPECT_EQ(journal.last_seq(), 3u);  // arm + both observations journaled
}

TEST_F(EngineTest, SecurityCriticalCommandNeedsFreshTargetState) {
  // No lock state ever reported: unknown freshness -> denied.
  auto r = engine.request(lock_request(kOwner, clock.now()));
  EXPECT_EQ(engine.state().commands.find(r.id)->status, CommandStatus::kDenied);

  engine.on_observation(observation(kLock, 1, LockState{false}, "lock.state"));
  Command again = lock_request(kOwner, clock.now());
  again.id = CommandId("lock-2");
  again.idempotency_key = "lock-2";
  auto ok = engine.request(again);
  EXPECT_EQ(engine.state().commands.find(ok.id)->status, CommandStatus::kDispatched);
}

TEST_F(EngineTest, AgentCannotUnlockAndUnknownPrincipalDenied) {
  engine.on_observation(observation(kLock, 1, LockState{true}, "lock.state"));
  auto agent = engine.request(lock_request(kAgent, clock.now()));
  EXPECT_EQ(engine.state().commands.find(agent.id)->status, CommandStatus::kDenied);
  auto ghost = engine.request(lock_request(PrincipalId("ghost"), clock.now()));
  EXPECT_EQ(engine.state().commands.find(ghost.id)->history.back().reason, "unknown_principal");
  EXPECT_TRUE(engine.take_outbound().empty());
}

TEST_F(EngineTest, RestartRebuildsIdenticalStateAndDedup) {
  engine.on_observation(testing::motion("node-07", 1, 1));
  clock.advance(Millis{400});
  engine.on_observation(observation(kSpeaker, 1, AudioPlayback{true}, "audio.playback_started"));
  engine.on_observation(testing::contact("node-12", 1, 1, true));
  clock.advance(Millis{9'000});
  engine.tick();

  HubEngine restarted(config(), journal, clock);
  EXPECT_EQ(restarted.state(), engine.state());
  EXPECT_EQ(restarted.on_observation(testing::motion("node-07", 1, 1)).status,
            IngestStatus::kDuplicate);
  EXPECT_TRUE(restarted.take_outbound().empty());
}

TEST_F(EngineTest, ApplyConfigSwapsRulesAndIsJournaled) {
  // Remove the backyard rule: motion no longer speaks.
  auto cfg = config();
  ASSERT_TRUE(engine.apply_config(2, "abc", kOwner, cfg.property, {}, cfg.principals));
  EXPECT_EQ(engine.state().config_revision, 2u);
  engine.on_observation(testing::motion("node-07", 1, 1));
  EXPECT_TRUE(engine.take_outbound().empty());
  // Stale or property-changing revisions are refused.
  EXPECT_EQ(engine.apply_config(2, "x", kOwner, cfg.property, {}, cfg.principals).error().code,
            "stale_revision");
  Property other = cfg.property;
  other.id = PropertyId("house-02");
  EXPECT_FALSE(engine.apply_config(3, "x", kOwner, other, {}, cfg.principals));
  // Replay reproduces the revision.
  EXPECT_EQ(replay({}, journal).config_revision, 2u);
}

TEST_F(EngineTest, PreviewMatchesWhatTheEngineDid) {
  engine.on_observation(testing::motion("node-07", 1, 1));  // armed: fires
  engine.on_observation(testing::motion("node-07", 1, 2, false));
  ASSERT_TRUE(engine.set_arm_mode(ArmMode::kDisarmed, kOwner));
  engine.on_observation(testing::motion("node-07", 1, 3));  // disarmed: no
  engine.on_observation(testing::motion("node-07", 1, 5, false));
  engine.on_observation(testing::motion("node-07", 1, 4));  // late: superseded
  ASSERT_TRUE(engine.set_arm_mode(ArmMode::kArmedAway, kOwner));
  engine.on_observation(testing::motion("node-12", 1, 1));  // hallway: wrong zone

  auto result =
      preview_rule(backyard_rule(), config().property, read_all(journal), Timestamp{0},
                   Timestamp{1'000'000}, [](Timestamp) -> std::uint16_t { return 2 * 60; });
  ASSERT_EQ(result.matches.size(), 1u);
  EXPECT_EQ(result.matches[0].event_id, EventId("node-07:1:1"));
  EXPECT_EQ(result.evaluated_observations, 6u);
  // Exactly the one command the engine dispatched.
  EXPECT_EQ(engine.state().commands.records().size(), 1u);
}

}  // namespace
}  // namespace talos
