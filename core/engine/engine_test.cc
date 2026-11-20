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
    if (fail) return Error{"storage_error", "disk full"};
    return inner.append(t, std::move(e));
  }
  std::vector<JournalEntry> read(std::uint64_t after, std::size_t limit) const override {
    return inner.read(after, limit);
  }
  std::uint64_t last_seq() const override { return inner.last_seq(); }
  bool fail = false;
  InMemoryJournal inner;
};

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
