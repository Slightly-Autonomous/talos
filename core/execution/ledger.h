// Durable command ledger as a pure state machine.
//
// Decision methods (request, dispatch, on_ack, on_observation, tick, cancel)
// never mutate; they return the transitions that should happen. The caller
// journals them and then calls apply(). Replaying the journaled transitions
// through apply() rebuilds an identical ledger.
#pragma once

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
};

class CommandLedger {
 public:
  RequestResult request(const Command& command, const Authorization& auth, Timestamp now) const;
  // Fails if the command is unknown or not in `accepted`. Returns either a
  // kDispatched transition (caller must then send) or kExpired (never send).
  Result<std::vector<CommandTransition>> dispatch(const CommandId& id, Timestamp now) const;
  // Fails for unknown commands or ACKs for commands that were never dispatched.
  Result<std::vector<CommandTransition>> on_ack(const CommandAck& ack, Timestamp now) const;
  std::vector<CommandTransition> on_observation(const ObservationEnvelope& obs,
                                                Timestamp received_at) const;
  // Closes dispatch deadlines and verification windows that have elapsed.
  std::vector<CommandTransition> tick(Timestamp now) const;
  Result<std::vector<CommandTransition>> cancel(const CommandId& id, Timestamp now) const;

  void apply(const CommandTransition& t);
  void apply_all(const std::vector<CommandTransition>& ts) {
    for (const auto& t : ts) apply(t);
  }

  const CommandRecord* find(const CommandId& id) const;
  const std::map<CommandId, CommandRecord>& records() const { return records_; }
  bool operator==(const CommandLedger&) const = default;

 private:
  std::map<CommandId, CommandRecord> records_;
  // Keyed by (principal, idempotency key): one principal's key never matches
  // another principal's command.
  std::map<std::pair<PrincipalId, std::string>, CommandId> by_idempotency_key_;
};

}  // namespace talos
