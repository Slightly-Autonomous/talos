#include "core/engine/engine.h"

#include <algorithm>
#include <utility>

#include "core/execution/operations.h"

namespace talos {

HubEngine::HubEngine(EngineConfig config, IJournal& journal, const IClock& clock)
    : config_(std::move(config)),
      journal_(journal),
      clock_(clock),
      ingestor_(config_.property, journal_) {
  ingestor_.restore();
  state_ = replay({}, journal_);
  // Accepted-but-not-handed-off commands survive a restart; their TTL still
  // gates dispatch.
  std::vector<const CommandRecord*> accepted;
  for (const auto& [id, rec] : state_.commands.records()) {
    if (rec.status == CommandStatus::kAccepted) accepted.push_back(&rec);
  }
  std::sort(accepted.begin(), accepted.end(), [](const auto* a, const auto* b) {
    return std::pair{a->command.created_at, a->command.id} <
           std::pair{b->command.created_at, b->command.id};
  });
  for (const auto* rec : accepted) pending_.push_back(rec->command.id);
}

Status HubEngine::journal(DomainEvent event) {
  auto entry = journal_.append(clock_.now(), std::move(event));
  if (!entry.ok()) return entry.error();
  state_ = reduce(std::move(state_), entry.value());
  return Status::Ok();
}

Status HubEngine::journal_all(const std::vector<CommandTransition>& ts) {
  for (const auto& t : ts) {
    if (Status s = journal(t); !s) return s;
  }
  return Status::Ok();
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

IngestResult HubEngine::on_observation(const ObservationEnvelope& env, IngestOptions options) {
  IngestResult r = ingestor_.ingest(env, clock_.now(), options);
  if (r.status != IngestStatus::kAccepted) return r;
  state_ = reduce(std::move(state_), *r.entry);

  // Only an observation that became current state (newest source position,
  // ADR 0008) is evidence or a trigger. A late, superseded one is history:
  // it must neither verify a command against newer state nor fire rules.
  auto it = state_.capabilities.find(env.source);
  if (it == state_.capabilities.end() || it->second.journal_seq != r.entry->journal_seq) {
    r.superseded = true;
    if (it != state_.capabilities.end()) r.held_position = it->second.position;
    return r;
  }

  // Evidence for pending commands, using hub receipt time.
  if (Status s = journal_all(state_.commands.on_observation(env, r.entry->received_at)); !s) {
    r.error = s.error();
    return r;
  }

  const RuleContext ctx{state_.arm_mode, config_.local_minute_of_day
                                             ? config_.local_minute_of_day(clock_.now())
                                             : std::uint16_t{0}};
  for (const auto& intent : evaluate_rules(config_.rules, config_.property, env, ctx)) {
    Command c = to_command(intent, clock_.now());
    RequestResult req = submit(c, Origin::kRule);
    if (req.conflict) {
      // Another principal holds this id (only possible with a journal from
      // before ids were reserved). Never drop a rule action silently: record
      // it under a fallback id; its idempotency key still dedupes it.
      ++r.rule_conflicts;
      c.id = CommandId(c.id.str() + ":conflict-" + std::to_string(r.entry->journal_seq));
      req = submit(c, Origin::kRule);
      if (req.conflict) {
        r.error = Error{"rule_command_conflict", "rule command id " + c.id.str() + " is held"};
        return r;
      }
    }
    if (req.error) {
      r.error = req.error;
      return r;
    }
  }
  return r;
}

RequestResult HubEngine::request(const Command& command, bool record_denial) {
  return submit(command, Origin::kRequest, record_denial);
}

Authorization HubEngine::authorize(const Command& command, Origin origin,
                                   Command& normalized) const {
  const Principal* principal = find_principal(command.principal);
  if (!principal) return {false, "unknown_principal"};
  // Rules always act as capped kRule principals (invariant 11), never as a
  // person whose id a rule happens to name.
  if (origin == Origin::kRule && principal->kind != PrincipalKind::kRule) {
    return {false, "rule_principal_must_be_rule_kind"};
  }
  // Catalog first: the operation must agree with args, target, TTL and
  // expectation before policy reasons about its risk.
  auto checked = normalize_command(command);
  if (!checked) return {false, checked.error().code};
  normalized = std::move(checked).value();

  const Device* device = config_.property.find_device(normalized.target.device);
  if (!device) return {false, "unknown_target_device"};
  if (!device->declares(normalized.target.capability)) return {false, "undeclared_capability"};

  // Freshness is always judged on what the command acts on.
  const Freshness f = freshness_of(normalized.target);
  return evaluate(*principal, normalized, {clock_.now(), f == Freshness::kFresh});
}

RequestResult HubEngine::submit(const Command& command, Origin origin, bool record_denial) {
  const Timestamp now = clock_.now();
  Command normalized = command;
  const Authorization auth = authorize(command, origin, normalized);
  // Denied requests are recorded as asked, for audit; accepted ones as checked.
  RequestResult req = state_.commands.request(auth.allowed ? normalized : command, auth, now);
  if (req.duplicate || req.conflict) return req;
  if (!auth.allowed && !record_denial) {
    RequestResult out{command.id, false, false, {}, {}, false, {}};
    out.unrecorded_denial = true;
    out.denied_reason = auth.reason;
    return out;
  }
  if (Status s = journal_all(req.transitions); !s) {
    req.error = s.error();
    return req;
  }
  const CommandRecord* rec = state_.commands.find(command.id);
  if (rec && rec->status == CommandStatus::kAccepted) pending_.push_back(command.id);
  return req;
}

std::vector<Command> HubEngine::hand_off(const DeviceId* device) {
  const Timestamp now = clock_.now();
  std::vector<Command> out;
  std::vector<CommandId> keep;
  for (std::size_t i = 0; i < pending_.size(); ++i) {
    const CommandId& id = pending_[i];
    const CommandRecord* rec = state_.commands.find(id);
    if (!rec || rec->status != CommandStatus::kAccepted) continue;  // expired/canceled
    if (device && rec->command.target.device != *device) {
      keep.push_back(id);
      continue;
    }
    auto ts = state_.commands.dispatch(id, now);
    if (!ts.ok()) continue;
    if (!journal_all(ts.value())) {
      // Fail closed: never hand over a command the journal does not show as
      // dispatched. It stays pending (or expires) for a later attempt.
      keep.insert(keep.end(), pending_.begin() + static_cast<std::ptrdiff_t>(i), pending_.end());
      break;
    }
    rec = state_.commands.find(id);
    if (rec && rec->status == CommandStatus::kDispatched) out.push_back(rec->command);
  }
  pending_ = std::move(keep);
  return out;
}

std::vector<Command> HubEngine::take_outbound() { return hand_off(nullptr); }

std::vector<Command> HubEngine::take_outbound(const DeviceId& device) { return hand_off(&device); }

Result<std::vector<CommandTransition>> HubEngine::on_ack(const CommandAck& ack) {
  auto ts = state_.commands.on_ack(ack, clock_.now());
  if (!ts.ok()) return ts;
  if (Status s = journal_all(ts.value()); !s) return s.error();
  return ts;
}

Status HubEngine::tick() {
  Status s = journal_all(state_.commands.tick(clock_.now()));
  std::erase_if(pending_, [&](const CommandId& id) {
    const CommandRecord* rec = state_.commands.find(id);
    return !rec || rec->status != CommandStatus::kAccepted;
  });
  return s;
}

Status HubEngine::set_arm_mode(ArmMode mode, const PrincipalId& principal_id) {
  const Principal* principal = find_principal(principal_id);
  if (!principal) return Error{"unknown_principal", principal_id.str()};
  Authorization auth = authorize_arm_change(*principal, config_.property.id, state_.arm_mode, mode);
  if (!auth.allowed) return Error{"denied", auth.reason};
  if (state_.arm_mode != mode) return journal(ArmModeChanged{mode, principal_id});
  return Status::Ok();
}

Status HubEngine::reset_source(const DeviceId& device, const PrincipalId& principal) {
  if (!config_.property.find_device(device)) return Error{"not_found", device.str()};
  if (Status s = journal(SourceReset{device, principal}); !s) return s;
  ingestor_.reset(device);
  return Status::Ok();
}

Status HubEngine::apply_config(std::uint32_t revision, const std::string& sha256,
                               const PrincipalId& principal, Property property,
                               std::vector<Rule> rules, std::vector<Principal> principals) {
  if (property.id != config_.property.id) {
    return Error{"invalid_config", "a revision cannot change the property id"};
  }
  if (Status s = validate_property(property); !s) return s;
  for (const auto& r : rules) {
    if (r.property != property.id)
      return Error{"invalid_config", "rule " + r.id.str() + " targets another property"};
  }
  if (revision <= state_.config_revision) {
    return Error{"stale_revision", "revision " + std::to_string(revision) + " is not newer"};
  }
  if (Status s = journal(ConfigRevisionApplied{revision, sha256, principal}); !s) return s;
  // Assign in place: the ingestor holds a reference to config_.property.
  config_.property = std::move(property);
  config_.rules = std::move(rules);
  config_.principals = std::move(principals);
  return Status::Ok();
}

}  // namespace talos
