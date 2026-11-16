// A miniature hub loop built only from core pieces. Phase 3 replaces this
// harness with the real server; the semantics asserted here must not change.
#include <gtest/gtest.h>

#include "core/execution/ledger.h"
#include "core/ingest/ingestor.h"
#include "core/journal/journal.h"
#include "core/policy/policy.h"
#include "core/rules/rules.h"
#include "core/state/state.h"
#include "core/testing/fixtures.h"

namespace talos {
namespace {

const CapabilityRef kSpeaker{DeviceId("patio_speaker"), std::nullopt, CapabilityId("audio.speak")};

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
  speak.ttl = Millis{5'000};
  speak.verification_window = Millis{3'000};
  speak.expect = ExpectedObservation{kSpeaker, AudioPlayback{true}};
  r.actions.push_back(speak);
  return r;
}

ObservationEnvelope speaker_playing(std::uint64_t seq) {
  ObservationEnvelope e;
  e.property = PropertyId("house-01");
  e.source = kSpeaker;
  e.event_type = "audio.playback_started";
  e.event_id = EventId("patio_speaker:1:" + std::to_string(seq));
  e.position = {1, seq};
  e.payload = AudioPlayback{true};
  return e;
}

// Hub-like orchestration: every decision is journaled before it takes effect.
class MiniHub {
 public:
  MiniHub(const Property& property, IJournal& journal)
      : property_(property), journal_(journal), ingestor_(property, journal) {}

  // Returns command ids that should be sent to adapters.
  std::vector<CommandId> on_observation(const ObservationEnvelope& env, Timestamp now) {
    std::vector<CommandId> to_send;
    auto r = ingestor_.ingest(env, now);
    if (r.status != IngestStatus::kAccepted) return to_send;
    state_ = reduce(std::move(state_), *r.entry);
    journal_transitions(state_.commands.on_observation(env, r.entry->received_at), now);

    for (const auto& intent : evaluate_rules(rules_, property_, env, context_)) {
      Command cmd = to_command(intent, now);
      Authorization auth = evaluate(rule_principal_, cmd, {now, std::nullopt});
      auto req = state_.commands.request(cmd, auth, now);
      if (req.duplicate) continue;
      journal_transitions(req.transitions, now);
      if (state_.commands.find(cmd.id)->status != CommandStatus::kAccepted) continue;
      auto dispatched = state_.commands.dispatch(cmd.id, now);
      if (!dispatched.ok()) continue;
      journal_transitions(dispatched.value(), now);
      if (state_.commands.find(cmd.id)->status == CommandStatus::kDispatched) {
        to_send.push_back(cmd.id);
      }
    }
    return to_send;
  }

  void tick(Timestamp now) { journal_transitions(state_.commands.tick(now), now); }

  void journal_transitions(const std::vector<CommandTransition>& ts, Timestamp now) {
    for (const auto& t : ts) {
      auto entry = journal_.append(now, t);
      state_ = reduce(std::move(state_), entry.value());
    }
  }

  const PropertyState& state() const { return state_; }
  RuleContext context_{ArmMode::kArmedAway, 2 * 60};

 private:
  const Property& property_;
  IJournal& journal_;
  Ingestor ingestor_;
  PropertyState state_;
  std::vector<Rule> rules_{backyard_rule()};
  Principal rule_principal_{PrincipalId("rule:backyard"),
                            PrincipalKind::kRule,
                            {{PropertyId("house-01"), RiskClass::kDisruptiveOutput}}};
};

class Phase1Slice : public ::testing::Test {
 protected:
  Property property = testing::sample_property();
  InMemoryJournal journal;
  MiniHub hub{property, journal};
};

TEST_F(Phase1Slice, BackyardIntrusionSpeaksAndVerifies) {
  auto sent = hub.on_observation(testing::motion("node-07", 1, 1), Timestamp{1'000});
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(hub.state().commands.find(sent[0])->status, CommandStatus::kDispatched);

  hub.on_observation(speaker_playing(1), Timestamp{1'400});
  EXPECT_EQ(hub.state().commands.find(sent[0])->status, CommandStatus::kVerified);
}

TEST_F(Phase1Slice, DuplicateMotionDeliveryCausesOnePhysicalEffect) {
  auto first = hub.on_observation(testing::motion("node-07", 1, 1), Timestamp{1'000});
  auto retry = hub.on_observation(testing::motion("node-07", 1, 1), Timestamp{1'050});
  EXPECT_EQ(first.size(), 1u);
  EXPECT_TRUE(retry.empty());
  EXPECT_EQ(hub.state().commands.records().size(), 1u);
}

TEST_F(Phase1Slice, SpeakerSilenceEndsUnknownNotSuccess) {
  auto sent = hub.on_observation(testing::motion("node-07", 1, 1), Timestamp{1'000});
  hub.tick(Timestamp{4'001});
  EXPECT_EQ(outcome_of(hub.state().commands.find(sent[0])->status), CommandOutcome::kUnknown);
}

TEST_F(Phase1Slice, NothingHappensWhenDisarmed) {
  hub.context_.mode = ArmMode::kDisarmed;
  EXPECT_TRUE(hub.on_observation(testing::motion("node-07", 1, 1), Timestamp{1'000}).empty());
  EXPECT_TRUE(hub.state().commands.records().empty());
}

TEST_F(Phase1Slice, RestartRebuildsIdenticalStateFromJournal) {
  hub.on_observation(testing::motion("node-07", 1, 1), Timestamp{1'000});
  hub.on_observation(speaker_playing(1), Timestamp{1'400});
  hub.on_observation(testing::contact("node-12", 1, 1, true), Timestamp{2'000});
  hub.tick(Timestamp{9'000});

  PropertyState rebuilt = replay({}, journal);
  EXPECT_EQ(rebuilt, hub.state());

  // After restart, a redelivered motion event is still recognized as a duplicate.
  Ingestor probe(property, journal);
  probe.restore();
  EXPECT_EQ(probe.ingest(testing::motion("node-07", 1, 1), Timestamp{10'000}).status,
            IngestStatus::kDuplicate);
}

TEST_F(Phase1Slice, CommandThatCannotBeDispatchedInTimeExpires) {
  Command c;
  c.id = CommandId("cmd-late");
  c.principal = PrincipalId("user-1");
  c.property = PropertyId("house-01");
  c.target = kSpeaker;
  c.operation = "audio.speak";
  c.created_at = Timestamp{0};
  c.expires_at = Timestamp{5'000};
  CommandLedger ledger;
  ledger.apply_all(ledger.request(c, {true, "ok"}, Timestamp{0}).transitions);
  // Sleepy device never opened a receive window before the TTL.
  ledger.apply_all(ledger.tick(Timestamp{5'000}));
  EXPECT_EQ(ledger.find(c.id)->status, CommandStatus::kExpired);
  EXPECT_FALSE(ledger.dispatch(c.id, Timestamp{6'000}).ok());
}

}  // namespace
}  // namespace talos
