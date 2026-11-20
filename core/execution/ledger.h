// Durable command ledger as a pure state machine.
//
// Decision methods (request, dispatch, on_ack, on_observation, tick, cancel)
// never mutate; they return the transitions that should happen. The caller
// journals them and then calls apply(). Replaying the journaled transitions
// through apply() rebuilds an identical ledger.
#pragma once

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/base/result.h"
#include "core/execution/command.h"

namespace talos {

struct CommandRecord {
  Command command;
  CommandStatus status = CommandStatus::kRequested;
  std::optional<Timestamp> dispatched_at;
  // The target's source position when the command was dispatched (derived
  // by the reducer, not journaled). Only a strictly newer report from the
  // target can verify the command (ADR 0004 amendment).
  std::optional<SourcePosition> baseline;
  std::vector<CommandTransition> history;

  Timestamp verification_deadline() const;
  bool operator==(const CommandRecord&) const = default;
};

struct RequestResult {
  CommandId id;            // existing id when duplicate
  bool duplicate = false;  // same principal reused an idempotency key or command id
  // The command id already belongs to another principal's command: nothing
  // was recorded and nothing about that command is revealed.
  bool conflict = false;
  std::vector<CommandTransition> transitions;
  // Set when journaling failed; the request was not (fully) recorded.
  std::optional<Error> error;
  // Denied, and deliberately not recorded (audit budget spent); the reason
  // is in `denied_reason`.
  bool unrecorded_denial = false;
  std::string denied_reason;
};

class CommandLedger {
 public:
  RequestResult request(const Command& command, const Authorization& auth, Timestamp now) const;
  // Fails if the command is unknown or not in `accepted`. Returns either a
  // kDispatched transition (caller must then send) or kExpired (never send).
  Result<std::vector<CommandTransition>> dispatch(const CommandId& id, Timestamp now) const;
  // Fails for unknown commands or ACKs for commands that were never dispatched.
  Result<std::vector<CommandTransition>> on_ack(const CommandAck& ack, Timestamp now) const;
  // `obs` must be an observation that became the target's current state (the
  // engine filters out superseded ones). It verifies a command only when its
  // source position is newer than the command's dispatch baseline.
  std::vector<CommandTransition> on_observation(const ObservationEnvelope& obs,
                                                Timestamp received_at) const;
  // Closes dispatch deadlines and verification windows that have elapsed.
  std::vector<CommandTransition> tick(Timestamp now) const;
  Result<std::vector<CommandTransition>> cancel(const CommandId& id, Timestamp now) const;

  void apply(const CommandTransition& t);
  void apply_all(const std::vector<CommandTransition>& ts) {
    for (const auto& t : ts) apply(t);
  }

  // Live (and non-denied terminal) commands first, then denied ones.
  const CommandRecord* find(const CommandId& id) const;
  // The principal's own command with that id, denied or not.
  const CommandRecord* find(const PrincipalId& principal, const CommandId& id) const;
  const std::map<CommandId, CommandRecord>& records() const { return records_; }
  // Denied requests, newest kMaxDenied only, keyed by (principal, id): a
  // denied request never holds a command id against other principals.
  const std::map<std::pair<PrincipalId, CommandId>, CommandRecord>& denied() const {
    return denied_;
  }
  // Reducer hooks (derived state, deterministic in journal order).
  void set_baseline(const CommandId& id, std::optional<SourcePosition> position);
  void clear_baselines(const DeviceId& device);
  bool operator==(const CommandLedger&) const = default;

  static constexpr std::size_t kMaxDenied = 256;

 private:
  std::map<CommandId, CommandRecord> records_;
  std::map<std::pair<PrincipalId, CommandId>, CommandRecord> denied_;
  std::deque<std::pair<PrincipalId, CommandId>> denied_order_;
  // Keyed by (principal, idempotency key): one principal's key never matches
  // another principal's command.
  std::map<std::pair<PrincipalId, std::string>, CommandId> by_idempotency_key_;
};

}  // namespace talos
