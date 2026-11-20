#include "core/execution/ledger.h"

#include <gtest/gtest.h>

namespace talos {
namespace {

constexpr Timestamp kT0{1'000'000};
const Authorization kAllow{true, "test grant"};

const CapabilityRef kLock{DeviceId("lock-3"), std::nullopt, CapabilityId("access.lock")};

Command lock_command(std::string id = "cmd-1") {
  Command c;
  c.id = CommandId(id);
  c.idempotency_key = "user-33:front_door:lock:true:rev4";
  c.principal = PrincipalId("user-33");
  c.property = PropertyId("house-01");
  c.target = kLock;
  c.operation = "access.set_locked";
  c.args = SetLocked{true};
  c.created_at = kT0;
  c.expires_at = kT0 + Millis{5'000};
  c.verification = VerificationMode::kObservedState;
  c.verification_window = Millis{3'000};
  c.expect = ExpectedObservation{kLock, LockState{true}};
  return c;
}

ObservationEnvelope lock_observation(bool locked, std::string id = "lock-3:1:9") {
  ObservationEnvelope o;
  o.property = PropertyId("house-01");
  o.source = kLock;
  o.event_type = "lock.state";
  o.event_id = EventId(id);
  o.position = {1, 9};
  o.payload = LockState{locked};
  return o;
}

// Applies decision output and returns it, mimicking journal-then-apply.
template <typename T>
std::vector<CommandTransition> commit(CommandLedger& l, T&& result) {
  if constexpr (requires { result.ok(); }) {
    EXPECT_TRUE(result.ok()) << result.error().message;
    auto ts = result.value();
    l.apply_all(ts);
    return ts;
  } else {
    l.apply_all(result);
    return result;
  }
}

CommandStatus status(const CommandLedger& l, const std::string& id = "cmd-1") {
  return l.find(CommandId(id))->status;
}

TEST(Ledger, HappyPathVerifiesOnlyOnObservedState) {
  CommandLedger l;
  commit(l, l.request(lock_command(), kAllow, kT0).transitions);
  EXPECT_EQ(status(l), CommandStatus::kAccepted);
  commit(l, l.dispatch(CommandId("cmd-1"), kT0 + Millis{100}));
  commit(l, l.on_ack({CommandId("cmd-1"), true, "ok"}, kT0 + Millis{200}));
  EXPECT_EQ(status(l), CommandStatus::kDeviceAcknowledged);
  EXPECT_NE(outcome_of(status(l)), CommandOutcome::kVerified);  // ACK is not verification
  commit(l, l.on_observation(lock_observation(true), kT0 + Millis{400}));
  EXPECT_EQ(status(l), CommandStatus::kVerified);
  EXPECT_EQ(outcome_of(status(l)), CommandOutcome::kVerified);
}

TEST(Ledger, ExpiredCommandIsNeverDispatched) {
  CommandLedger l;
  commit(l, l.request(lock_command(), kAllow, kT0).transitions);
  auto r = l.dispatch(CommandId("cmd-1"), kT0 + Millis{5'000});
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r.value().size(), 1u);
  EXPECT_EQ(r.value()[0].to, CommandStatus::kExpired);
  l.apply_all(r.value());
  // Any later dispatch attempt is refused.
  EXPECT_EQ(l.dispatch(CommandId("cmd-1"), kT0 + Millis{5'001}).error().code, "not_dispatchable");
  EXPECT_FALSE(l.find(CommandId("cmd-1"))->dispatched_at.has_value());
}

TEST(Ledger, TickExpiresUndispatchedCommands) {
  CommandLedger l;
  commit(l, l.request(lock_command(), kAllow, kT0).transitions);
  EXPECT_TRUE(l.tick(kT0 + Millis{4'999}).empty());
  commit(l, l.tick(kT0 + Millis{5'000}));
  EXPECT_EQ(status(l), CommandStatus::kExpired);
}

TEST(Ledger, RequestAlreadyPastTtlExpiresImmediately) {
  CommandLedger l;
  commit(l, l.request(lock_command(), kAllow, kT0 + Millis{6'000}).transitions);
  EXPECT_EQ(status(l), CommandStatus::kExpired);
}

TEST(Ledger, DeniedCommandIsTerminal) {
  CommandLedger l;
  commit(l, l.request(lock_command(), Authorization{false, "no grant"}, kT0).transitions);
  EXPECT_EQ(status(l), CommandStatus::kDenied);
  EXPECT_FALSE(l.dispatch(CommandId("cmd-1"), kT0).ok());
}

TEST(Ledger, SameIdempotencyKeyYieldsOneCommand) {
  CommandLedger l;
  auto first = l.request(lock_command("cmd-1"), kAllow, kT0);
  l.apply_all(first.transitions);
  auto retry = l.request(lock_command("cmd-2"), kAllow, kT0 + Millis{10});
  EXPECT_TRUE(retry.duplicate);
  EXPECT_EQ(retry.id, CommandId("cmd-1"));
  EXPECT_TRUE(retry.transitions.empty());
  EXPECT_EQ(l.records().size(), 1u);
}

TEST(Ledger, DuplicateAndOutOfOrderAcksAreIdempotent) {
  CommandLedger l;
  commit(l, l.request(lock_command(), kAllow, kT0).transitions);
  commit(l, l.dispatch(CommandId("cmd-1"), kT0));
  // Observation beats the ACK: verified directly.
  commit(l, l.on_observation(lock_observation(true), kT0 + Millis{100}));
  EXPECT_EQ(status(l), CommandStatus::kVerified);
  // ACK arrives afterwards, twice: no change.
  auto a1 = l.on_ack({CommandId("cmd-1"), true, ""}, kT0 + Millis{200});
  auto a2 = l.on_ack({CommandId("cmd-1"), true, ""}, kT0 + Millis{300});
  EXPECT_TRUE(a1.value().empty());
  EXPECT_TRUE(a2.value().empty());
  EXPECT_EQ(status(l), CommandStatus::kVerified);
}

TEST(Ledger, NoEvidenceInWindowIsUnknownNotSuccess) {
  CommandLedger l;
  commit(l, l.request(lock_command(), kAllow, kT0).transitions);
  commit(l, l.dispatch(CommandId("cmd-1"), kT0));
  commit(l, l.tick(kT0 + Millis{3'001}));
  EXPECT_EQ(status(l), CommandStatus::kUnknownResult);
  EXPECT_EQ(outcome_of(status(l)), CommandOutcome::kUnknown);
}

TEST(Ledger, AckWithoutObservationIsCompletedUnverifiedNeverSuccess) {
  CommandLedger l;
  commit(l, l.request(lock_command(), kAllow, kT0).transitions);
  commit(l, l.dispatch(CommandId("cmd-1"), kT0));
  commit(l, l.on_ack({CommandId("cmd-1"), true, ""}, kT0 + Millis{100}));
  commit(l, l.tick(kT0 + Millis{3'001}));
  EXPECT_EQ(status(l), CommandStatus::kCompletedUnverified);
  EXPECT_EQ(outcome_of(status(l)), CommandOutcome::kUnverified);  // invariant #6
}

TEST(Ledger, LateAckIsRecordedFaithfully) {
  CommandLedger l;
  commit(l, l.request(lock_command(), kAllow, kT0).transitions);
  commit(l, l.dispatch(CommandId("cmd-1"), kT0));
  commit(l, l.tick(kT0 + Millis{3'001}));
  ASSERT_EQ(status(l), CommandStatus::kUnknownResult);

  auto late = commit(l, l.on_ack({CommandId("cmd-1"), true, "executed"}, kT0 + Millis{9'000}));
  ASSERT_FALSE(late.empty());
  EXPECT_EQ(late[0].to, CommandStatus::kDeviceAcknowledged);
  EXPECT_TRUE(late[0].late);
  // The device did act: outcome moves off "unknown" to "acked but unverified".
  EXPECT_EQ(status(l), CommandStatus::kCompletedUnverified);
}

TEST(Ledger, LateRejectionIsRecordedAsRejection) {
  CommandLedger l;
  commit(l, l.request(lock_command(), kAllow, kT0).transitions);
  commit(l, l.dispatch(CommandId("cmd-1"), kT0));
  commit(l, l.tick(kT0 + Millis{3'001}));
  auto late = commit(l, l.on_ack({CommandId("cmd-1"), false, "jammed"}, kT0 + Millis{8'000}));
  EXPECT_EQ(status(l), CommandStatus::kRejected);
  EXPECT_TRUE(late[0].late);
}

TEST(Ledger, LateObservationVerifiesWithLateFlag) {
  CommandLedger l;
  commit(l, l.request(lock_command(), kAllow, kT0).transitions);
  commit(l, l.dispatch(CommandId("cmd-1"), kT0));
  commit(l, l.on_ack({CommandId("cmd-1"), true, ""}, kT0 + Millis{100}));
  commit(l, l.tick(kT0 + Millis{3'001}));
  auto v = commit(l, l.on_observation(lock_observation(true), kT0 + Millis{7'000}));
  ASSERT_EQ(v.size(), 1u);
  EXPECT_TRUE(v[0].late);
  EXPECT_EQ(v[0].evidence, EventId("lock-3:1:9"));
  EXPECT_EQ(status(l), CommandStatus::kVerified);
}

TEST(Ledger, MismatchedOrStaleObservationDoesNotVerify) {
  CommandLedger l;
  commit(l, l.request(lock_command(), kAllow, kT0).transitions);
  commit(l, l.dispatch(CommandId("cmd-1"), kT0 + Millis{1'000}));
  EXPECT_TRUE(l.on_observation(lock_observation(false), kT0 + Millis{1'500}).empty());
  // A matching observation received before dispatch is not evidence.
  EXPECT_TRUE(l.on_observation(lock_observation(true), kT0 + Millis{999}).empty());
}

TEST(Ledger, AckForUndispatchedCommandIsAnError) {
  CommandLedger l;
  commit(l, l.request(lock_command(), kAllow, kT0).transitions);
  EXPECT_EQ(l.on_ack({CommandId("cmd-1"), true, ""}, kT0).error().code,
            "ack_for_undispatched_command");
  EXPECT_EQ(l.on_ack({CommandId("nope"), true, ""}, kT0).error().code, "unknown_command");
}

TEST(Ledger, AckOnlyModeNeverReachesVerified) {
  CommandLedger l;
  Command c = lock_command();
  c.operation = "notify";
  c.args = Notify{"owner", "motion"};
  c.verification = VerificationMode::kAckOnly;
  c.expect.reset();
  commit(l, l.request(c, kAllow, kT0).transitions);
  commit(l, l.dispatch(c.id, kT0));
  commit(l, l.on_ack({c.id, true, ""}, kT0 + Millis{10}));
  EXPECT_EQ(status(l), CommandStatus::kCompletedUnverified);
}

TEST(Ledger, AckOnlyCommandCarryingAnExpectStillNeverVerifies) {
  CommandLedger l;
  Command c = lock_command();
  c.verification = VerificationMode::kAckOnly;  // expect left set on purpose
  commit(l, l.request(c, kAllow, kT0).transitions);
  commit(l, l.dispatch(c.id, kT0));
  EXPECT_TRUE(l.on_observation(lock_observation(true), kT0 + Millis{10}).empty());
  commit(l, l.on_ack({c.id, true, ""}, kT0 + Millis{20}));
  EXPECT_EQ(status(l), CommandStatus::kCompletedUnverified);
}

TEST(Ledger, IdempotencyKeysAreScopedToThePrincipal) {
  CommandLedger l;
  commit(l, l.request(lock_command("cmd-1"), kAllow, kT0).transitions);
  // Another principal using the same key gets its own command, not cmd-1.
  Command other = lock_command("cmd-2");
  other.principal = PrincipalId("agent-1");
  auto r = l.request(other, kAllow, kT0);
  EXPECT_FALSE(r.duplicate);
  EXPECT_FALSE(r.conflict);
  commit(l, r.transitions);
  EXPECT_EQ(l.records().size(), 2u);
  // Reusing another principal's command id is a conflict that reveals nothing.
  Command steal = lock_command("cmd-1");
  steal.principal = PrincipalId("agent-1");
  steal.idempotency_key = "fresh";
  auto c = l.request(steal, kAllow, kT0);
  EXPECT_TRUE(c.conflict);
  EXPECT_FALSE(c.duplicate);
  EXPECT_TRUE(c.transitions.empty());
}

TEST(Ledger, CancelOnlyBeforeDispatch) {
  CommandLedger l;
  commit(l, l.request(lock_command(), kAllow, kT0).transitions);
  commit(l, l.cancel(CommandId("cmd-1"), kT0));
  EXPECT_EQ(status(l), CommandStatus::kCanceled);

  CommandLedger l2;
  commit(l2, l2.request(lock_command(), kAllow, kT0).transitions);
  commit(l2, l2.dispatch(CommandId("cmd-1"), kT0));
  EXPECT_EQ(l2.cancel(CommandId("cmd-1"), kT0).error().code, "not_cancelable");
}

TEST(Ledger, ReplayingTransitionsRebuildsIdenticalLedger) {
  CommandLedger live;
  std::vector<CommandTransition> journal;
  auto record = [&](std::vector<CommandTransition> ts) {
    live.apply_all(ts);
    journal.insert(journal.end(), ts.begin(), ts.end());
  };
  record(live.request(lock_command(), kAllow, kT0).transitions);
  record(live.dispatch(CommandId("cmd-1"), kT0).value());
  record(live.on_ack({CommandId("cmd-1"), true, ""}, kT0 + Millis{50}).value());
  record(live.tick(kT0 + Millis{4'000}));

  CommandLedger rebuilt;
  rebuilt.apply_all(journal);
  const auto& a = *live.find(CommandId("cmd-1"));
  const auto& b = *rebuilt.find(CommandId("cmd-1"));
  EXPECT_EQ(a.status, b.status);
  EXPECT_EQ(a.dispatched_at, b.dispatched_at);
  EXPECT_EQ(a.history.size(), b.history.size());
  // Idempotency index is rebuilt too.
  EXPECT_TRUE(rebuilt.request(lock_command("cmd-9"), kAllow, kT0).duplicate);
}

}  // namespace
}  // namespace talos
