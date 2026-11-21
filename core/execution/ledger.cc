#include "core/execution/ledger.h"

namespace talos {

Timestamp CommandRecord::verification_deadline() const {
  return dispatched_at.value_or(command.expires_at) + command.verification_window;
}

namespace {

CommandTransition make(const CommandId& id, CommandStatus to, Timestamp at, std::string reason,
                       bool late = false) {
  CommandTransition t;
  t.command = id;
  t.to = to;
  t.at = at;
  t.reason = std::move(reason);
  t.late = late;
  return t;
}

bool awaiting_evidence(CommandStatus s) {
  return s == CommandStatus::kDispatched || s == CommandStatus::kDeviceAcknowledged ||
         s == CommandStatus::kCompletedUnverified || s == CommandStatus::kUnknownResult;
}

}  // namespace

RequestResult CommandLedger::request(const Command& command, const Authorization& auth,
                                     Timestamp now) const {
  if (auto it = by_idempotency_key_.find({command.principal, command.idempotency_key});
      !command.idempotency_key.empty() && it != by_idempotency_key_.end()) {
    return {it->second, true, false, {}, {}, false, {}};
  }
  if (auto it = records_.find(command.id); it != records_.end()) {
    if (it->second.command.principal != command.principal)
      return {command.id, false, true, {}, {}, false, {}};
    return {command.id, true, false, {}, {}, false, {}};
  }
  if (denied_.contains({command.principal, command.id}))
    return {command.id, true, false, {}, {}, false, {}};

  RequestResult r{command.id, false, false, {}, {}, false, {}};
  CommandTransition created = make(command.id, CommandStatus::kRequested, now, "requested");
  created.created = command;
  r.transitions.push_back(std::move(created));
  if (!auth.allowed) {
    r.transitions.push_back(make(command.id, CommandStatus::kDenied, now, auth.reason));
  } else if (now >= command.expires_at) {
    r.transitions.push_back(
        make(command.id, CommandStatus::kExpired, now, "expired before authorization completed"));
  } else {
    r.transitions.push_back(make(command.id, CommandStatus::kAccepted, now, auth.reason));
  }
  return r;
}

Result<std::vector<CommandTransition>> CommandLedger::dispatch(const CommandId& id,
                                                               Timestamp now) const {
  const CommandRecord* rec = find(id);
  if (!rec) return Error{"unknown_command", id.str()};
  if (rec->status != CommandStatus::kAccepted) {
    return Error{"not_dispatchable",
                 "command " + id.str() + " is " + std::string(to_string(rec->status))};
  }
  if (now >= rec->command.expires_at) {
    return std::vector{make(id, CommandStatus::kExpired, now, "ttl elapsed before dispatch")};
  }
  return std::vector{make(id, CommandStatus::kDispatched, now, "dispatched")};
}

Result<std::vector<CommandTransition>> CommandLedger::complete_internal(
    const CommandId& id, Timestamp now, std::string evidence) const {
  const CommandRecord* rec = find(id);
  if (!rec) return Error{"unknown_command", id.str()};
  if (rec->status != CommandStatus::kAccepted) {
    return Error{"not_dispatchable",
                 "command " + id.str() + " is " + std::string(to_string(rec->status))};
  }
  return std::vector{make(id, CommandStatus::kDispatched, now, "executed by the hub"),
                     make(id, CommandStatus::kVerified, now, std::move(evidence))};
}

Result<std::vector<CommandTransition>> CommandLedger::on_ack(const CommandAck& ack,
                                                             Timestamp now) const {
  const CommandRecord* rec = find(ack.command);
  if (!rec) return Error{"unknown_command", ack.command.str()};
  if (!rec->dispatched_at) {
    return Error{"ack_for_undispatched_command",
                 "command " + ack.command.str() + " is " + std::string(to_string(rec->status))};
  }
  const bool late = now > rec->verification_deadline();
  std::vector<CommandTransition> out;
  switch (rec->status) {
    case CommandStatus::kDispatched:
    case CommandStatus::kUnknownResult:
      if (!ack.accepted) {
        out.push_back(make(ack.command, CommandStatus::kRejected, now, ack.detail, late));
        break;
      }
      out.push_back(make(ack.command, CommandStatus::kDeviceAcknowledged, now, ack.detail, late));
      if (rec->command.verification == VerificationMode::kAckOnly) {
        out.push_back(make(ack.command, CommandStatus::kCompletedUnverified, now,
                           "ack-only verification mode", late));
      } else if (late) {
        // Window already closed: the ACK is real, but nothing confirmed the state.
        out.push_back(make(ack.command, CommandStatus::kCompletedUnverified, now,
                           "late ack; no confirming observation in window", true));
      }
      break;
    default:
      // Already acknowledged, verified, rejected or completed: duplicate or
      // out-of-order ACK. Idempotent no-op.
      break;
  }
  return out;
}

std::vector<CommandTransition> CommandLedger::on_observation(const ObservationEnvelope& obs,
                                                             Timestamp received_at) const {
  std::vector<CommandTransition> out;
  for (const auto& [id, rec] : records_) {
    if (!awaiting_evidence(rec.status) || !rec.command.expect || !rec.dispatched_at) continue;
    // Ack-only commands never become verified, whatever they carry (ADR 0004).
    if (rec.command.verification == VerificationMode::kAckOnly) continue;
    const auto& expect = *rec.command.expect;
    if (expect.source != obs.source || expect.payload != obs.payload) continue;
    if (received_at < *rec.dispatched_at) continue;  // predates the command
    // A report the device made before (or at) dispatch says nothing about
    // what the command did (ADR 0004 amendment).
    if (rec.baseline && !(*rec.baseline < obs.position)) continue;
    auto t = make(id, CommandStatus::kVerified, received_at, "observed expected state",
                  received_at > rec.verification_deadline());
    t.evidence = obs.event_id;
    out.push_back(std::move(t));
  }
  return out;
}

std::vector<CommandTransition> CommandLedger::tick(Timestamp now) const {
  std::vector<CommandTransition> out;
  for (const auto& [id, rec] : records_) {
    if (rec.status == CommandStatus::kAccepted && now >= rec.command.expires_at) {
      out.push_back(make(id, CommandStatus::kExpired, now, "ttl elapsed before dispatch"));
    } else if (rec.dispatched_at && now > rec.verification_deadline()) {
      if (rec.status == CommandStatus::kDispatched) {
        out.push_back(make(id, CommandStatus::kUnknownResult, now,
                           "verification window closed with no ack or observation"));
      } else if (rec.status == CommandStatus::kDeviceAcknowledged) {
        out.push_back(make(id, CommandStatus::kCompletedUnverified, now,
                           "verification window closed; ack without confirming observation"));
      }
    }
  }
  return out;
}

Result<std::vector<CommandTransition>> CommandLedger::cancel(const CommandId& id,
                                                             Timestamp now) const {
  const CommandRecord* rec = find(id);
  if (!rec) return Error{"unknown_command", id.str()};
  if (rec->status != CommandStatus::kAccepted && rec->status != CommandStatus::kRequested) {
    return Error{"not_cancelable",
                 "command " + id.str() + " is " + std::string(to_string(rec->status))};
  }
  return std::vector{make(id, CommandStatus::kCanceled, now, "canceled")};
}

void CommandLedger::apply(const CommandTransition& t) {
  if (t.created) {
    CommandRecord rec;
    rec.command = *t.created;
    rec.status = CommandStatus::kRequested;
    rec.history.push_back(t);
    if (!rec.command.idempotency_key.empty()) {
      by_idempotency_key_.emplace(std::pair{rec.command.principal, rec.command.idempotency_key},
                                  t.command);
    }
    records_.emplace(t.command, std::move(rec));
    return;
  }
  auto it = records_.find(t.command);
  if (it == records_.end()) return;  // journal invariant: creation precedes transitions
  it->second.status = t.to;
  if (t.to == CommandStatus::kDispatched) it->second.dispatched_at = t.at;
  it->second.history.push_back(t);
  if (t.to != CommandStatus::kDenied) return;
  // Denied requests move out of the id namespace and are kept bounded.
  auto node = records_.extract(it);
  std::pair key{node.mapped().command.principal, t.command};
  denied_.insert_or_assign(key, std::move(node.mapped()));
  denied_order_.push_back(key);
  while (denied_order_.size() > kMaxDenied) {
    const auto old = denied_order_.front();
    denied_order_.pop_front();
    auto d = denied_.find(old);
    if (d == denied_.end()) continue;
    const auto& key_str = d->second.command.idempotency_key;
    if (auto k = by_idempotency_key_.find({old.first, key_str});
        k != by_idempotency_key_.end() && k->second == old.second) {
      by_idempotency_key_.erase(k);
    }
    denied_.erase(d);
  }
}

void CommandLedger::set_baseline(const CommandId& id, std::optional<SourcePosition> position) {
  if (auto it = records_.find(id); it != records_.end()) it->second.baseline = position;
}

void CommandLedger::clear_baselines(const DeviceId& device) {
  for (auto& [id, rec] : records_) {
    if (rec.command.expect && rec.command.expect->source.device == device) rec.baseline.reset();
  }
}

const CommandRecord* CommandLedger::find(const CommandId& id) const {
  if (auto it = records_.find(id); it != records_.end()) return &it->second;
  for (const auto& [key, rec] : denied_) {
    if (key.second == id) return &rec;
  }
  return nullptr;
}

const CommandRecord* CommandLedger::find(const PrincipalId& principal, const CommandId& id) const {
  if (auto it = records_.find(id); it != records_.end()) {
    return it->second.command.principal == principal ? &it->second : nullptr;
  }
  auto it = denied_.find({principal, id});
  return it == denied_.end() ? nullptr : &it->second;
}

}  // namespace talos
