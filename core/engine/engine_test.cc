#include "core/engine/engine.h"

#include <gtest/gtest.h>

#include "core/engine/preview.h"
#include "core/execution/operations.h"
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
  ASSERT_TRUE(engine.tick());
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
  // Accepted, not dispatched: hand-off to the transport is the dispatch.
  EXPECT_EQ(engine.state().commands.find(ok.id)->status, CommandStatus::kAccepted);
  ASSERT_EQ(engine.take_outbound().size(), 1u);
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
  ASSERT_TRUE(engine.tick());

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

// --- Red team (2026-11-20) ----------------------------------------------------------------

// #1: an older, reordered report must not verify a command while the newer
// state says the opposite.
TEST_F(EngineTest, OutOfOrderOlderObservationNeverVerifies) {
  engine.on_observation(observation(kLock, 6, LockState{false}, "lock.state"));
  auto r = engine.request(lock_request(kOwner, clock.now()));
  ASSERT_EQ(engine.take_outbound().size(), 1u);
  clock.advance(Millis{200});
  auto late = engine.on_observation(observation(kLock, 4, LockState{true}, "lock.state"));
  EXPECT_EQ(late.status, IngestStatus::kAccepted);
  EXPECT_TRUE(late.superseded);
  EXPECT_NE(engine.state().commands.find(r.id)->status, CommandStatus::kVerified);
  EXPECT_EQ(std::get<LockState>(engine.state().capabilities.at(kLock).value).locked, false);
  // A newer report that does show the expected state verifies it.
  engine.on_observation(observation(kLock, 7, LockState{true}, "lock.state"));
  EXPECT_EQ(engine.state().commands.find(r.id)->status, CommandStatus::kVerified);
  EXPECT_EQ(replay({}, journal), engine.state());
}

// #1: verification needs a report newer than where the target stood at dispatch.
TEST_F(EngineTest, ReportNotNewerThanDispatchBaselineNeverVerifies) {
  engine.on_observation(observation(kLock, 6, LockState{false}, "lock.state"));
  auto r = engine.request(lock_request(kOwner, clock.now()));
  ASSERT_EQ(engine.take_outbound().size(), 1u);
  // An unseen older report (seq 5 was never delivered) that says "locked": it
  // fills a gap but is older than the baseline, so it is not evidence.
  engine.on_observation(observation(kLock, 5, LockState{true}, "lock.state"));
  EXPECT_NE(engine.state().commands.find(r.id)->status, CommandStatus::kVerified);
}

// #2: a rule's command id held by another principal never drops the action.
TEST_F(EngineTest, RuleCommandIdHeldByAnotherPrincipalStillActs) {
  Command squat;
  squat.id = CommandId("cmd:backyard_motion_warning@1:node-07:1:1:0");
  squat.idempotency_key = squat.id.str();
  squat.principal = kOwner;
  squat.property = PropertyId("house-01");
  squat.target = kSpeaker;
  squat.operation = "audio.speak";
  squat.args = AudioSpeak{"hi", 10};
  squat.created_at = clock.now();
  squat.expires_at = clock.now() + Millis{5'000};
  ASSERT_EQ(engine.request(squat).transitions.size(), 2u);  // accepted under the owner
  auto r = engine.on_observation(testing::motion("node-07", 1, 1));
  EXPECT_EQ(r.rule_conflicts, 1u);
  EXPECT_FALSE(r.error.has_value());
  bool rule_command = false;
  for (const auto& [id, rec] : engine.state().commands.records()) {
    rule_command |= rec.command.principal == PrincipalId("rule:backyard") &&
                    rec.status == CommandStatus::kAccepted;
  }
  EXPECT_TRUE(rule_command);
}

// #4: a forged huge boot epoch is refused, and an owner reset recovers a
// device whose real reports fell behind a (bounded) poisoned position.
TEST_F(EngineTest, SourceResetRecoversAPoisonedDevice) {
  auto poison = testing::motion("node-07", 0xFFFF'FFFF, 1, false);
  EXPECT_EQ(engine.on_observation(poison).error->code, "source_position_out_of_range");
  engine.on_observation(testing::motion("node-07", 5, 1, false));
  auto bounded = engine.on_observation(testing::motion("node-07", 60, 1, false));
  ASSERT_EQ(bounded.status, IngestStatus::kAccepted);
  // The real device (epoch 6) is now history only: no rule fires.
  auto real = engine.on_observation(testing::motion("node-07", 6, 1));
  EXPECT_TRUE(real.superseded);
  ASSERT_TRUE(real.held_position.has_value());
  EXPECT_EQ(real.held_position->boot_epoch, 60u);
  EXPECT_TRUE(engine.take_outbound().empty());

  ASSERT_TRUE(engine.reset_source(DeviceId("node-07"), kOwner));
  auto heard = engine.on_observation(testing::motion("node-07", 6, 2));
  EXPECT_FALSE(heard.superseded);
  EXPECT_EQ(engine.take_outbound().size(), 1u);  // the rule fires again
  EXPECT_EQ(replay({}, journal), engine.state());
  // A restart rebuilds the same dedup state, reset included.
  HubEngine restarted{config(), journal, clock};
  EXPECT_EQ(restarted.on_observation(testing::motion("node-07", 6, 2)).status,
            IngestStatus::kDuplicate);
  EXPECT_EQ(restarted.on_observation(testing::motion("node-07", 6, 3)).status,
            IngestStatus::kAccepted);
}

// #8: once a principal's audit budget is spent, denials are answered but not
// journaled.
TEST_F(EngineTest, UnrecordedDenialLeavesNoTrace) {
  const auto before = journal.last_seq();
  auto r = engine.request(lock_request(kAgent, clock.now()), /*record_denial=*/false);
  EXPECT_TRUE(r.unrecorded_denial);
  EXPECT_FALSE(r.denied_reason.empty());
  EXPECT_EQ(journal.last_seq(), before);
  EXPECT_EQ(engine.state().commands.find(r.id), nullptr);
}

// #7 through the engine: an agent cannot step away -> home.
TEST_F(EngineTest, AgentCannotLowerArmMode) {
  EXPECT_EQ(engine.set_arm_mode(ArmMode::kArmedHome, kAgent).error().message,
            "requires_human_authorization");
  EXPECT_EQ(engine.state().arm_mode, ArmMode::kArmedAway);
  ASSERT_TRUE(engine.set_arm_mode(ArmMode::kArmedHome, kOwner));
  EXPECT_TRUE(engine.set_arm_mode(ArmMode::kArmedAway, kAgent));
}

// --- Operation catalog (B1) ---------------------------------------------------------------

Command request_as(PrincipalId who, std::string id, std::string operation, CapabilityRef target,
                   CommandArgs args, Timestamp now) {
  Command c;
  c.id = CommandId(id);
  c.idempotency_key = id;
  c.principal = who;
  c.property = PropertyId("house-01");
  c.target = std::move(target);
  c.operation = std::move(operation);
  c.args = std::move(args);
  c.created_at = now;
  c.expires_at = now + Millis{5'000};
  return c;
}

const std::string& reason_of(const HubEngine& e, const CommandId& id) {
  return e.state().commands.find(id)->history.back().reason;
}

TEST_F(EngineTest, LowRiskOperationNameCannotCarryUnlockArgs) {
  engine.on_observation(observation(kLock, 1, LockState{true}, "lock.state"));
  // audio.speak is within the agent's cap, but the args would unlock the door.
  auto r = engine.request(
      request_as(kAgent, "sneaky", "audio.speak", kLock, SetLocked{false}, clock.now()));
  EXPECT_EQ(engine.state().commands.find(r.id)->status, CommandStatus::kDenied);
  EXPECT_EQ(reason_of(engine, r.id), "args_do_not_match_operation");
  // Matching args but a lock target: still refused before policy.
  auto t = engine.request(
      request_as(kAgent, "sneaky-2", "audio.speak", kLock, AudioSpeak{"hi", 50}, clock.now()));
  EXPECT_EQ(reason_of(engine, t.id), "target_does_not_match_operation");
  auto u = engine.request(
      request_as(kAgent, "sneaky-3", "door.open", kLock, SetLocked{false}, clock.now()));
  EXPECT_EQ(reason_of(engine, u.id), "unsupported_operation");
  EXPECT_TRUE(engine.take_outbound().empty());
  EXPECT_TRUE(engine.take_outbound(kLock.device).empty());
}

// --- Hand-off is the dispatch (B2) ---------------------------------------------------------

TEST_F(EngineTest, CommandNotHandedOffWithinTtlExpiresAndIsNeverSent) {
  engine.on_observation(observation(kLock, 1, LockState{false}, "lock.state"));
  auto r = engine.request(lock_request(kOwner, clock.now()));
  ASSERT_EQ(engine.state().commands.find(r.id)->status, CommandStatus::kAccepted);
  // Another device's poll does not hand it off.
  EXPECT_TRUE(engine.take_outbound(kSpeaker.device).empty());
  clock.advance(Millis{5'000});  // adapter offline past the TTL
  EXPECT_TRUE(engine.take_outbound(kLock.device).empty());
  EXPECT_EQ(engine.state().commands.find(r.id)->status, CommandStatus::kExpired);
  EXPECT_FALSE(engine.state().commands.find(r.id)->dispatched_at.has_value());
}

TEST_F(EngineTest, DispatchTimeIsHandOffTimeAndSurvivesRestart) {
  engine.on_observation(observation(kLock, 1, LockState{false}, "lock.state"));
  auto r = engine.request(lock_request(kOwner, clock.now()));
  clock.advance(Millis{1'000});
  HubEngine restarted(config(), journal, clock);  // accepted command is still pending
  auto out = restarted.take_outbound(kLock.device);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(restarted.state().commands.find(r.id)->dispatched_at, clock.now());
  EXPECT_TRUE(restarted.take_outbound(kLock.device).empty());
}

TEST_F(EngineTest, TickExpiresUnsentCommandsSoNoLaterPollDeliversThem) {
  engine.on_observation(observation(kLock, 1, LockState{false}, "lock.state"));
  auto r = engine.request(lock_request(kOwner, clock.now()));
  clock.advance(Millis{60'000});
  ASSERT_TRUE(engine.tick());
  EXPECT_EQ(engine.state().commands.find(r.id)->status, CommandStatus::kExpired);
  EXPECT_TRUE(engine.take_outbound().empty());
}

// --- Freshness and verification are the hub's, not the caller's (B3) ----------------------

TEST_F(EngineTest, CallerExpectCannotMoveTheFreshnessCheckOffTheTarget) {
  const CapabilityRef health{kLock.device, std::nullopt, CapabilityId("device.health")};
  engine.on_observation(observation(health, 1, Heartbeat{3'000}, "device.heartbeat"));
  // Lock state never reported. Pointing expect at the fresh heartbeat must not help.
  Command c = lock_request(kOwner, clock.now());
  c.args = SetLocked{false};
  c.expect = ExpectedObservation{health, Heartbeat{3'000}};
  auto r = engine.request(c);
  EXPECT_EQ(engine.state().commands.find(r.id)->status, CommandStatus::kDenied);
  EXPECT_EQ(reason_of(engine, r.id), "expectation_does_not_match_operation");
  // Without the bogus expect it is the target's staleness that denies it.
  Command plain = c;
  plain.id = CommandId("plain");
  plain.idempotency_key = "plain";
  plain.expect.reset();
  auto p = engine.request(plain);
  EXPECT_EQ(reason_of(engine, p.id), "target_state_not_fresh");
}

TEST_F(EngineTest, OnlyTheDerivedObservationVerifiesACommand) {
  const CapabilityRef health{kLock.device, std::nullopt, CapabilityId("device.health")};
  engine.on_observation(observation(kLock, 1, LockState{true}, "lock.state"));
  Command c = lock_request(kOwner, clock.now());
  c.args = SetLocked{false};
  c.expect.reset();  // the hub derives lock{locked:false} on the target
  auto r = engine.request(c);
  ASSERT_EQ(engine.take_outbound(kLock.device).size(), 1u);
  const auto& recorded = engine.state().commands.find(r.id)->command;
  ASSERT_TRUE(recorded.expect.has_value());
  EXPECT_EQ(recorded.expect->source, kLock);
  EXPECT_EQ(recorded.expect->payload, ObservationPayload{LockState{false}});
  clock.advance(Millis{100});
  engine.on_observation(observation(health, 2, Heartbeat{3'000}, "device.heartbeat"));
  EXPECT_NE(engine.state().commands.find(r.id)->status, CommandStatus::kVerified);
  engine.on_observation(observation(kLock, 3, LockState{false}, "lock.state"));
  EXPECT_EQ(engine.state().commands.find(r.id)->status, CommandStatus::kVerified);
}

// --- Rule principals (M1) -------------------------------------------------------------------

TEST_F(EngineTest, RuleNamingAHumanPrincipalIsDenied) {
  auto cfg = config();
  cfg.rules[0].principal = kOwner;  // a rule must never act as the owner
  ASSERT_TRUE(engine.apply_config(2, "x", kOwner, cfg.property, cfg.rules, cfg.principals));
  engine.on_observation(testing::motion("node-07", 1, 1));
  EXPECT_TRUE(engine.take_outbound().empty());
  // Denied requests are kept apart from the id namespace (red team #2).
  EXPECT_TRUE(engine.state().commands.records().empty());
  ASSERT_EQ(engine.state().commands.denied().size(), 1u);
  const auto& rec = engine.state().commands.denied().begin()->second;
  EXPECT_EQ(rec.status, CommandStatus::kDenied);
  EXPECT_EQ(rec.history.back().reason, "rule_principal_must_be_rule_kind");
}

// --- Request bounds and targets (m1, m11) --------------------------------------------------

TEST_F(EngineTest, TtlWindowAndTargetAreBounded) {
  engine.on_observation(observation(kLock, 1, LockState{false}, "lock.state"));
  Command long_ttl = lock_request(kOwner, clock.now());
  long_ttl.expires_at = clock.now() + Millis{60'000};  // > 30 s for security-critical
  EXPECT_EQ(reason_of(engine, engine.request(long_ttl).id), "ttl_out_of_bounds");

  Command long_window =
      request_as(kOwner, "w", "audio.speak", kSpeaker, AudioSpeak{"x", 50}, clock.now());
  long_window.verification_window = Millis{3'600'000};
  EXPECT_EQ(reason_of(engine, engine.request(long_window).id), "verification_window_out_of_bounds");

  const CapabilityRef ghost{DeviceId("ghost"), std::nullopt, CapabilityId("audio.speak")};
  auto g = engine.request(
      request_as(kOwner, "g", "audio.speak", ghost, AudioSpeak{"x", 50}, clock.now()));
  EXPECT_EQ(reason_of(engine, g.id), "unknown_target_device");
}

// --- Enclosure tamper (HD-032, ADR 0007 amendment) -----------------------------------------

const CapabilityRef kTamper{DeviceId("node-12"), std::nullopt, CapabilityId("tamper.enclosure")};
const CapabilityRef kNotifier{DeviceId("notifier"), std::nullopt, CapabilityId("notify")};

// Tamper is a 24-hour condition: the rule has no armed_modes, so it alarms in
// every mode, disarmed included (alarm-panel convention).
EngineConfig tamper_config() {
  EngineConfig c = config();
  for (auto& d : c.property.devices) {
    if (d.id == DeviceId("node-12")) d.capabilities.emplace_back("tamper.enclosure");
  }
  Device n;
  n.id = DeviceId("notifier");
  n.property = c.property.id;
  n.capabilities = {CapabilityId("notify")};
  c.property.devices.push_back(n);
  Rule r;
  r.id = RuleId("tamper_alarm");
  r.property = c.property.id;
  r.principal = PrincipalId("rule:tamper");
  r.trigger = {"tamper.opened", std::nullopt};
  RuleAction a;
  a.operation = "notify";
  a.target = kNotifier;
  a.args = Notify{"owner", "Sensor node enclosure opened"};
  a.verification = VerificationMode::kAckOnly;
  r.actions.push_back(a);
  c.rules.push_back(r);
  c.principals.push_back({PrincipalId("rule:tamper"),
                          PrincipalKind::kRule,
                          {{c.property.id, RiskClass::kBenignOutput}}});
  return c;
}

class TamperTest : public ::testing::Test {
 protected:
  InMemoryJournal journal;
  VirtualClock clock{Timestamp{1'000}};
  HubEngine engine{tamper_config(), journal, clock};
};

TEST_F(TamperTest, OpeningAlarmsInEveryModeAndIsJournaled) {
  std::uint64_t seq = 0;
  for (ArmMode m : {ArmMode::kDisarmed, ArmMode::kArmedHome, ArmMode::kArmedAway}) {
    ASSERT_TRUE(engine.set_arm_mode(m, kOwner));
    const auto r =
        engine.on_observation(observation(kTamper, ++seq, TamperState{true}, "tamper.opened"));
    EXPECT_EQ(r.status, IngestStatus::kAccepted);
    auto out = engine.take_outbound();
    ASSERT_EQ(out.size(), 1u) << int(m);
    EXPECT_EQ(out[0].operation, "notify");
    EXPECT_EQ(out[0].causation, EventId("node-12:1:" + std::to_string(seq)));
    engine.on_observation(observation(kTamper, ++seq, TamperState{false}, "tamper.closed"));
    EXPECT_TRUE(engine.take_outbound().empty());  // closing is recorded, not an alarm
  }
  // Both directions are in the journal and the state, ordered by journal_seq.
  const auto* st = &engine.state().capabilities.at(kTamper);
  EXPECT_EQ(std::get<TamperState>(st->value), TamperState{false});
  int opened = 0, closed = 0;
  for (const auto& e : read_all(journal, 0)) {
    if (const auto* o = std::get_if<ObservationRecorded>(&e.event)) {
      opened += o->envelope.event_type == "tamper.opened";
      closed += o->envelope.event_type == "tamper.closed";
    }
  }
  EXPECT_EQ(opened, 3);
  EXPECT_EQ(closed, 3);
}

TEST_F(TamperTest, RedeliveredOpenAlarmsOnceAndBootStateReportDoesNotAlarm) {
  engine.on_observation(observation(kTamper, 1, TamperState{true}, "tamper.state"));
  EXPECT_TRUE(engine.take_outbound().empty());  // a boot report is a state, not an opening
  engine.on_observation(observation(kTamper, 2, TamperState{true}, "tamper.opened"));
  EXPECT_EQ(
      engine.on_observation(observation(kTamper, 2, TamperState{true}, "tamper.opened")).status,
      IngestStatus::kDuplicate);
  EXPECT_EQ(engine.take_outbound().size(), 1u);
}

TEST_F(TamperTest, OwnershipAndPayloadChecks) {
  // A device that does not declare tamper.enclosure cannot report it.
  CapabilityRef other{DeviceId("node-07"), std::nullopt, CapabilityId("tamper.enclosure")};
  EXPECT_EQ(
      engine.on_observation(observation(other, 1, TamperState{true}, "tamper.opened")).error->code,
      "undeclared_capability");
  // The event type must agree with the payload; the payload with the capability.
  EXPECT_EQ(engine.on_observation(observation(kTamper, 1, TamperState{false}, "tamper.opened"))
                .error->code,
            "event_type_does_not_match_payload");
  EXPECT_EQ(engine.on_observation(observation(kTamper, 2, ContactState{true}, "contact.opened"))
                .error->code,
            "payload_does_not_match_capability");
  EXPECT_TRUE(engine.take_outbound().empty());
}

// Red team: the node's P0 opening sat in retry backoff while its P1 restore
// overtook it. As superseded history the opening never alarmed.
TEST_F(TamperTest, LateOpeningOvertakenByItsRestoreStillAlarms) {
  engine.on_observation(observation(kTamper, 1, TamperState{false}, "tamper.state"));
  engine.on_observation(observation(kTamper, 3, TamperState{false}, "tamper.closed"));
  EXPECT_TRUE(engine.take_outbound().empty());
  const auto r = engine.on_observation(observation(kTamper, 2, TamperState{true}, "tamper.opened"));
  EXPECT_TRUE(r.superseded);
  auto out = engine.take_outbound();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].causation, EventId("node-12:1:2"));
  // Still history for state: the lid is closed.
  EXPECT_EQ(std::get<TamperState>(engine.state().capabilities.at(kTamper).value),
            TamperState{false});
  // A late closing is just history.
  engine.on_observation(observation(kTamper, 4, TamperState{true}, "tamper.opened"));
  engine.take_outbound();
  engine.on_observation(observation(kTamper, 5, TamperState{false}, "tamper.closed"));
  EXPECT_TRUE(engine.take_outbound().empty());
}

// Red team: lid opened, then the node lost power (or a watchdog reset) before
// the P0 left. After the cold boot it can only send a state report. The hub
// last knew the lid closed, so that report is an opening and must alarm.
TEST_F(TamperTest, ColdBootStateOpenAfterAKnownClosedLidAlarms) {
  engine.on_observation(observation(kTamper, 1, TamperState{false}, "tamper.state"));
  EXPECT_TRUE(engine.take_outbound().empty());
  ObservationEnvelope boot = observation(kTamper, 1, TamperState{true}, "tamper.state");
  boot.position = {2, 1};  // new boot epoch
  boot.event_id = EventId("node-12:2:1");
  engine.on_observation(boot);
  auto out = engine.take_outbound();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].causation, EventId("node-12:2:1"));
  // Already known open: another state report is not a second opening.
  ObservationEnvelope again = boot;
  again.position = {3, 1};
  again.event_id = EventId("node-12:3:1");
  engine.on_observation(again);
  EXPECT_TRUE(engine.take_outbound().empty());
}

// Red team: event ids name their source. node-07 could otherwise pre-claim
// node-12's next id; node-12's real opening would dedupe to node-07's command.
TEST_F(TamperTest, AnEventIdOfAnotherDeviceIsRefused) {
  ObservationEnvelope squat =
      observation({DeviceId("node-07"), std::nullopt, CapabilityId("contact.open_state")}, 1,
                  ContactState{true}, "contact.opened");
  squat.event_id = EventId("node-12:1:2");
  EXPECT_EQ(engine.on_observation(squat).error->code, "event_id_not_owned_by_source");
  squat.event_id = EventId("node-07");  // no separator: not namespaced either
  EXPECT_EQ(engine.on_observation(squat).error->code, "event_id_not_owned_by_source");
  squat.event_id = EventId("node-0:1:1");
  EXPECT_EQ(engine.on_observation(squat).error->code, "event_id_not_owned_by_source");
  engine.on_observation(observation(kTamper, 2, TamperState{true}, "tamper.opened"));
  EXPECT_EQ(engine.take_outbound().size(), 1u);
}

// --- Maintenance mode (owner decision 2026-11-21) ---------------------------------------

const CapabilityRef kTamper07{DeviceId("node-07"), std::nullopt, CapabilityId("tamper.enclosure")};

class MaintenanceTest : public ::testing::Test {
 protected:
  static EngineConfig make_config() {
    EngineConfig c = tamper_config();
    for (auto& d : c.property.devices) {
      if (d.id == DeviceId("node-07")) d.capabilities.emplace_back("tamper.enclosure");
    }
    return c;
  }
  void SetUp() override {
    ASSERT_TRUE(engine.set_arm_mode(ArmMode::kArmedAway, kOwner));
    heartbeat("node-12");
  }
  void heartbeat(const std::string& device) {
    engine.on_observation(
        observation({DeviceId(device), std::nullopt, CapabilityId("device.health")}, ++seq,
                    Heartbeat{3900}, "device.heartbeat"));
  }
  Command maintenance(const PrincipalId& who, std::uint32_t ms, const std::string& id,
                      const std::string& op = "device.maintenance") {
    Command c;
    c.id = CommandId(id);
    c.idempotency_key = id;
    c.principal = who;
    c.property = PropertyId("house-01");
    c.target = {DeviceId("node-12"), std::nullopt, CapabilityId("device.health")};
    c.operation = op;
    if (op == "device.maintenance") c.args = SetMaintenance{ms};
    c.created_at = clock.now();
    c.expires_at = clock.now() + Millis{10'000};
    return c;
  }
  void open(const CapabilityRef& ref, bool lid) {
    engine.on_observation(
        observation(ref, ++seq, TamperState{lid}, lid ? "tamper.opened" : "tamper.closed"));
  }
  int suppressed() {
    int n = 0;
    for (const auto& e : read_all(journal, 0))
      n += std::holds_alternative<AlarmSuppressed>(e.event);
    return n;
  }
  std::uint64_t seq = 100;
  InMemoryJournal journal;
  VirtualClock clock{Timestamp{1'000}};
  HubEngine engine{make_config(), journal, clock};
};

TEST_F(MaintenanceTest, SuppressesOnlyThatNodesTamperAlarmAndIsJournaled) {
  auto r = engine.request(maintenance(kOwner, 0, "m1"));
  EXPECT_EQ(engine.state().commands.find(r.id)->status, CommandStatus::kVerified);
  ASSERT_TRUE(engine.state().maintenance.contains(DeviceId("node-12")));
  EXPECT_EQ(engine.state().maintenance.at(DeviceId("node-12")).until,
            clock.now() + kDefaultMaintenance);  // 30 min default
  EXPECT_TRUE(engine.take_outbound().empty());   // never handed to a device
  open(kTamper, true);
  EXPECT_TRUE(engine.take_outbound().empty());
  EXPECT_EQ(std::get<TamperState>(engine.state().capabilities.at(kTamper).value),
            TamperState{true});
  ASSERT_EQ(suppressed(), 1);
  for (const auto& e : read_all(journal, 0)) {
    if (const auto* a = std::get_if<AlarmSuppressed>(&e.event)) {
      EXPECT_EQ(a->maintenance_by, kOwner);
      EXPECT_EQ(a->rule, RuleId("tamper_alarm"));
      EXPECT_EQ(a->maintenance_until, clock.now() + kDefaultMaintenance);
    }
  }
  open(kTamper07, true);  // another node: alarms as usual
  EXPECT_EQ(engine.take_outbound().size(), 1u);
}

TEST_F(MaintenanceTest, ExpiryWithTheLidOpenAlarmsOnceDeterministically) {
  engine.request(maintenance(kOwner, 60'000, "m1"));
  open(kTamper, true);
  clock.advance(Millis{59'999});
  ASSERT_TRUE(engine.tick());
  EXPECT_TRUE(engine.take_outbound().empty());
  clock.advance(Millis{1});
  ASSERT_TRUE(engine.tick());
  EXPECT_FALSE(engine.state().maintenance.contains(DeviceId("node-12")));
  auto out = engine.take_outbound();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].causation, EventId("node-12:1:" + std::to_string(seq)));
  ASSERT_TRUE(engine.tick());  // no repeat
  EXPECT_TRUE(engine.take_outbound().empty());
  // The expiry is journaled by the system, before the alarm.
  bool expired = false;
  for (const auto& e : read_all(journal, 0)) {
    if (const auto* m = std::get_if<MaintenanceChanged>(&e.event); m && !m->until) {
      expired = m->reason == "expired" && m->principal == kMaintenanceExpiry;
    }
  }
  EXPECT_TRUE(expired);
}

TEST_F(MaintenanceTest, LidClosedBeforeExpiryNeverAlarms) {
  engine.request(maintenance(kOwner, 60'000, "m1"));
  open(kTamper, true);
  open(kTamper, false);
  clock.advance(Millis{60'000});
  ASSERT_TRUE(engine.tick());
  EXPECT_TRUE(engine.take_outbound().empty());
}

TEST_F(MaintenanceTest, CancelWithTheLidOpenAlarmsAndAnAlarmAlreadyRaisedIsNotRepeated) {
  open(kTamper, true);  // alarms before maintenance
  ASSERT_EQ(engine.take_outbound().size(), 1u);
  engine.request(maintenance(kOwner, 60'000, "m1"));
  // Ending raises protection: allowed for an agent (benign), like arming.
  auto end = engine.request(maintenance(kAgent, 0, "m-end", "device.maintenance_end"));
  EXPECT_EQ(engine.state().commands.find(end.id)->status, CommandStatus::kVerified);
  EXPECT_FALSE(engine.state().maintenance.contains(DeviceId("node-12")));
  EXPECT_TRUE(engine.take_outbound().empty());  // same opening: deduped, not a second alarm
  engine.request(maintenance(kOwner, 60'000, "m2"));
  open(kTamper, false);
  open(kTamper, true);  // suppressed
  engine.request(maintenance(kOwner, 0, "m-end2", "device.maintenance_end"));
  EXPECT_EQ(engine.take_outbound().size(), 1u);  // cancelled with the lid open: alarm now
}

TEST_F(MaintenanceTest, OnlyHumansWithFreshStateAndBoundedDurations) {
  auto agent = engine.request(maintenance(kAgent, 0, "a1"));
  EXPECT_EQ(engine.state().commands.find(agent.id)->history.back().reason,
            "requires_human_authorization");
  auto too_long = engine.request(maintenance(kOwner, 4 * 3'600'000 + 1, "m-long"));
  EXPECT_EQ(engine.state().commands.find(too_long.id)->history.back().reason, "args_out_of_bounds");
  EXPECT_EQ(engine.state()
                .commands.find(engine.request(maintenance(kOwner, 59'999, "m-s")).id)
                ->history.back()
                .reason,
            "args_out_of_bounds");
  clock.advance(Millis{61'000});  // heartbeat older than freshness_max_age (60 s)
  auto stale = engine.request(maintenance(kOwner, 0, "m-stale"));
  EXPECT_EQ(engine.state().commands.find(stale.id)->history.back().reason,
            "target_state_not_fresh");
  EXPECT_TRUE(engine.state().maintenance.empty());
}

TEST_F(MaintenanceTest, RestartKeepsTheWindowAndItsExpiry) {
  engine.request(maintenance(kOwner, 60'000, "m1"));
  open(kTamper, true);
  HubEngine restarted(make_config(), journal, clock);
  EXPECT_EQ(restarted.state(), engine.state());
  EXPECT_TRUE(restarted.in_maintenance(DeviceId("node-12")));
  clock.advance(Millis{60'000});
  ASSERT_TRUE(restarted.tick());
  EXPECT_EQ(restarted.take_outbound().size(), 1u);
}

// Red team: back-to-back restarts kept an open lid silent forever. A chain of
// windows is capped at kMaxMaintenance from its first start.
TEST_F(MaintenanceTest, WindowsCannotBeChainedPastTheMaximum) {
  ASSERT_EQ(
      engine.state().commands.find(engine.request(maintenance(kOwner, 3'600'000, "m1")).id)->status,
      CommandStatus::kVerified);
  open(kTamper, true);  // suppressed
  int n = 0;
  for (; n < 10; ++n) {
    clock.advance(Millis{50 * 60'000});
    heartbeat("node-12");
    auto r = engine.request(maintenance(kOwner, 3'600'000, "m-chain-" + std::to_string(n)));
    if (engine.state().commands.find(r.id)->status != CommandStatus::kVerified) {
      EXPECT_EQ(engine.state().commands.find(r.id)->history.back().reason, "maintenance_limit");
      break;
    }
  }
  EXPECT_LT(n, 4);  // 4 h from the first start, whatever the restarts
  // A shorter restart inside the cap is fine.
  const auto since = engine.state().maintenance.at(DeviceId("node-12")).since;
  auto ok = engine.request(maintenance(kOwner, 60'000, "m-short"));
  EXPECT_EQ(engine.state().commands.find(ok.id)->status, CommandStatus::kVerified);
  EXPECT_EQ(engine.state().maintenance.at(DeviceId("node-12")).since, since);
  // At the cap the window ends and the still-open lid alarms.
  clock.advance(Millis{4 * 3'600'000});
  ASSERT_TRUE(engine.tick());
  EXPECT_EQ(engine.take_outbound().size(), 1u);
  // After it ended, a new window starts a new chain.
  heartbeat("node-12");
  auto fresh = engine.request(maintenance(kOwner, 3'600'000, "m-new"));
  EXPECT_EQ(engine.state().commands.find(fresh.id)->status, CommandStatus::kVerified);
  EXPECT_EQ(engine.state().maintenance.at(DeviceId("node-12")).since, clock.now());
  HubEngine restarted(make_config(), journal, clock);
  EXPECT_EQ(restarted.state(), engine.state());  // the chain start replays
}

// Maintenance of node-12 never hides node-12's other alarms (here: contact).
TEST_F(MaintenanceTest, OtherEventsOfTheNodeStillFireTheirRules) {
  EngineConfig c = make_config();
  Rule r;
  r.id = RuleId("door_open");
  r.property = c.property.id;
  r.principal = PrincipalId("rule:tamper");
  r.trigger = {"contact.opened", std::nullopt};
  RuleAction a;
  a.operation = "notify";
  a.target = kNotifier;
  a.args = Notify{"owner", "door"};
  a.verification = VerificationMode::kAckOnly;
  r.actions.push_back(a);
  c.rules.push_back(r);
  InMemoryJournal j;
  HubEngine e(c, j, clock);
  e.on_observation(observation({DeviceId("node-12"), std::nullopt, CapabilityId("device.health")},
                               1, Heartbeat{3900}, "device.heartbeat"));
  Command m = maintenance(kOwner, 0, "m1");
  ASSERT_EQ(e.state().commands.find(e.request(m).id)->status, CommandStatus::kVerified);
  CapabilityRef door{DeviceId("node-12"), SlotIndex{0}, CapabilityId("contact.open_state")};
  e.on_observation(observation(door, 2, ContactState{true}, "contact.opened"));
  EXPECT_EQ(e.take_outbound().size(), 1u);
  // And a late or cold-boot opening during maintenance is suppressed, not lost.
  e.on_observation(observation(kTamper, 4, TamperState{false}, "tamper.closed"));
  e.on_observation(observation(kTamper, 3, TamperState{true}, "tamper.opened"));
  EXPECT_TRUE(e.take_outbound().empty());
  int suppressed = 0;
  for (const auto& entry : read_all(j, 0)) {
    suppressed += std::holds_alternative<AlarmSuppressed>(entry.event);
  }
  EXPECT_EQ(suppressed, 1);
}

TEST(EngineCapabilities, UndeclaredTargetCapabilityIsDenied) {
  auto cfg = config();
  for (auto& d : cfg.property.devices) {
    if (d.id == DeviceId("patio_speaker")) d.capabilities = {CapabilityId("notify")};
  }
  InMemoryJournal journal;
  VirtualClock clock{Timestamp{1'000}};
  HubEngine engine{cfg, journal, clock};
  auto r = engine.request(
      request_as(kOwner, "s", "audio.speak", kSpeaker, AudioSpeak{"x", 50}, clock.now()));
  EXPECT_EQ(reason_of(engine, r.id), "undeclared_capability");
}

// --- Journal failures propagate (M6) -------------------------------------------------------

class FlakyJournal final : public IJournal {
 public:
  Result<JournalEntry> append(Timestamp t, DomainEvent e) override {
    if (fail || (fail_maintenance && std::holds_alternative<MaintenanceChanged>(e))) {
      return Error{"storage_error", "disk full"};
    }
    return inner.append(t, std::move(e));
  }
  std::vector<JournalEntry> read(std::uint64_t after, std::size_t limit) const override {
    return inner.read(after, limit);
  }
  std::uint64_t last_seq() const override { return inner.last_seq(); }
  bool fail = false;
  bool fail_maintenance = false;
  InMemoryJournal inner;
};

// Red team: a crash between accepting device.maintenance and journaling its
// effect left it accepted; on restart it was queued and handed to the node.
TEST(EngineJournal, AHubInternalCommandLeftAcceptedIsNeverHandedToADevice) {
  FlakyJournal journal;
  VirtualClock clock{Timestamp{1'000}};
  HubEngine engine{tamper_config(), journal, clock};
  engine.on_observation(
      observation({DeviceId("node-12"), std::nullopt, CapabilityId("device.health")}, 1,
                  Heartbeat{3900}, "device.heartbeat"));
  Command c;
  c.id = CommandId("m1");
  c.idempotency_key = "m1";
  c.principal = kOwner;
  c.property = PropertyId("house-01");
  c.target = {DeviceId("node-12"), std::nullopt, CapabilityId("device.health")};
  c.operation = "device.maintenance";
  c.args = SetMaintenance{0};
  c.created_at = clock.now();
  c.expires_at = clock.now() + Millis{10'000};
  journal.fail_maintenance = true;
  auto r = engine.request(c);
  ASSERT_TRUE(r.error.has_value());
  EXPECT_EQ(engine.state().commands.find(c.id)->status, CommandStatus::kAccepted);
  EXPECT_TRUE(engine.take_outbound().empty());
  journal.fail_maintenance = false;
  HubEngine restarted(tamper_config(), journal, clock);
  EXPECT_TRUE(restarted.take_outbound().empty());
  EXPECT_TRUE(restarted.take_outbound(DeviceId("node-12")).empty());
  clock.advance(Millis{10'001});
  ASSERT_TRUE(restarted.tick());
  EXPECT_EQ(restarted.state().commands.find(c.id)->status, CommandStatus::kExpired);
  EXPECT_TRUE(restarted.state().maintenance.empty());
}

TEST(EngineJournal, AppendFailuresPropagateAndNothingIsHandedOff) {
  FlakyJournal journal;
  VirtualClock clock{Timestamp{1'000}};
  HubEngine engine{config(), journal, clock};
  ASSERT_TRUE(engine.set_arm_mode(ArmMode::kArmedAway, kOwner));
  engine.on_observation(observation(kLock, 1, LockState{false}, "lock.state"));

  journal.fail = true;
  auto r = engine.request(lock_request(kOwner, clock.now()));  // used to dereference null
  ASSERT_TRUE(r.error.has_value());
  EXPECT_EQ(r.error->code, "storage_error");
  EXPECT_EQ(engine.state().commands.find(r.id), nullptr);
  EXPECT_FALSE(engine.set_arm_mode(ArmMode::kArmedHome, kOwner));
  EXPECT_EQ(engine.state().arm_mode, ArmMode::kArmedAway);
  auto cfg = config();
  EXPECT_FALSE(engine.apply_config(2, "x", kOwner, cfg.property, {}, cfg.principals));
  EXPECT_EQ(engine.config().rules.size(), 1u);  // unchanged

  journal.fail = false;
  Command ok = lock_request(kOwner, clock.now());
  ok.id = CommandId("ok");
  ok.idempotency_key = "ok";
  ASSERT_FALSE(engine.request(ok).error.has_value());
  journal.fail = true;
  EXPECT_TRUE(engine.take_outbound().empty());  // dispatch could not be journaled
  EXPECT_EQ(engine.state().commands.find(ok.id)->status, CommandStatus::kAccepted);
  clock.advance(Millis{10'000});
  EXPECT_FALSE(engine.tick());
}

}  // namespace
}  // namespace talos
