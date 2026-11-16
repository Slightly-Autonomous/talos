#include "core/engine/engine.h"

namespace talos {

HubEngine::HubEngine(EngineConfig config, IJournal& journal, const IClock& clock)
    : config_(std::move(config)),
      journal_(journal),
      clock_(clock),
      ingestor_(config_.property, journal_) {
  ingestor_.restore();
  state_ = replay({}, journal_);
}

void HubEngine::journal(DomainEvent event) {
  auto entry = journal_.append(clock_.now(), std::move(event));
  if (entry.ok()) state_ = reduce(std::move(state_), entry.value());
}

void HubEngine::journal_all(const std::vector<CommandTransition>& ts) {
  for (const auto& t : ts) journal(t);
}

const Principal* HubEngine::find_principal(const PrincipalId& id) const {
  for (const auto& p : config_.principals) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

Freshness HubEngine::freshness_of(const CapabilityRef& ref) const {
  return freshness(state_, ref, clock_.now(), config_.freshness_max_age);
}

IngestResult HubEngine::on_observation(const ObservationEnvelope& env) {
  IngestResult r = ingestor_.ingest(env, clock_.now());
  if (r.status != IngestStatus::kAccepted) return r;
  state_ = reduce(std::move(state_), *r.entry);

  // Evidence for pending commands, using hub receipt time.
  journal_all(state_.commands.on_observation(env, r.entry->received_at));

  // Rules react to state changes only: a late observation that did not become
  // current state (superseded at its source) is history, not a trigger.
  auto it = state_.capabilities.find(env.source);
  if (it == state_.capabilities.end() || it->second.journal_seq != r.entry->journal_seq) return r;

  const RuleContext ctx{state_.arm_mode, config_.local_minute_of_day
                                             ? config_.local_minute_of_day(clock_.now())
                                             : std::uint16_t{0}};
  for (const auto& intent : evaluate_rules(config_.rules, config_.property, env, ctx)) {
    submit(to_command(intent, clock_.now()), find_principal(intent.principal));
  }
  return r;
}

RequestResult HubEngine::request(const Command& command) {
  return submit(command, find_principal(command.principal));
}

RequestResult HubEngine::submit(const Command& command, const Principal* principal) {
  const Timestamp now = clock_.now();
  Authorization auth{false, "unknown_principal"};
  if (principal) {
    const CapabilityRef& state_ref = command.expect ? command.expect->source : command.target;
    const Freshness f = freshness_of(state_ref);
    auth = evaluate(*principal, command, {now, f == Freshness::kFresh});
  }
  RequestResult req = state_.commands.request(command, auth, now);
  if (req.duplicate) return req;
  journal_all(req.transitions);
  if (state_.commands.find(command.id)->status != CommandStatus::kAccepted) return req;

  auto dispatched = state_.commands.dispatch(command.id, now);
  if (!dispatched.ok()) return req;
  journal_all(dispatched.value());
  req.transitions.insert(req.transitions.end(), dispatched.value().begin(),
                         dispatched.value().end());
  if (state_.commands.find(command.id)->status == CommandStatus::kDispatched) {
    outbound_.push_back(command);
  }
  return req;
}

Result<std::vector<CommandTransition>> HubEngine::on_ack(const CommandAck& ack) {
  auto ts = state_.commands.on_ack(ack, clock_.now());
  if (!ts.ok()) return ts;
  journal_all(ts.value());
  return ts;
}

void HubEngine::tick() { journal_all(state_.commands.tick(clock_.now())); }

Status HubEngine::set_arm_mode(ArmMode mode, const PrincipalId& principal_id) {
  const Principal* principal = find_principal(principal_id);
  if (!principal) return Error{"unknown_principal", principal_id.str()};
  Authorization auth = authorize_arm_change(*principal, config_.property.id, mode);
  if (!auth.allowed) return Error{"denied", auth.reason};
  if (state_.arm_mode != mode) journal(ArmModeChanged{mode, principal_id});
  return Status::Ok();
}

std::vector<Command> HubEngine::take_outbound() {
  std::vector<Command> out;
  out.swap(outbound_);
  return out;
}

}  // namespace talos
