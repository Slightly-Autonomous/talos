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
  for (const auto* rec : accepted) {
    // A hub-internal command (device.maintenance*) left accepted by a crash
    // before its effect was journaled is never handed to a device: it expires
    // by its TTL instead of being dispatched to a node that can't run it.
    const OperationSpec* spec = find_operation(rec->command.operation);
    if (spec && spec->hub_internal) continue;
    pending_.push_back(rec->command.id);
  }
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
  // The hub's tamper state before this report: a boot state report of an open
  // lid that the hub last knew closed is an opening nobody reported.
  std::optional<bool> prior_tamper;
  if (std::holds_alternative<TamperState>(env.payload)) {
    if (auto it = state_.capabilities.find(env.source); it != state_.capabilities.end()) {
      if (const auto* t = std::get_if<TamperState>(&it->second.value)) prior_tamper = t->open;
    }
  }
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
    // Exception (ADR 0007 amendment): a late enclosure opening still alarms.
    // The node's P0 opening can sit in retry backoff while its P1 restore
    // overtakes it; as history it would never alarm, and a lid opened and
    // closed again would go unannounced. It still verifies nothing.
    if (env.event_type == "tamper.opened") run_rules(env, r);
    return r;
  }

  // Evidence for pending commands, using hub receipt time.
  if (Status s = journal_all(state_.commands.on_observation(env, r.entry->received_at)); !s) {
    r.error = s.error();
    return r;
  }

  if (const auto* t = std::get_if<TamperState>(&env.payload);
      t && t->open && env.event_type == "tamper.state" && prior_tamper == false) {
    // A cold boot (power loss, watchdog, brown-out) lost the opening's P0: the
    // node can only send its state. The hub saw the lid closed, so this is an
    // opening and alarms as one, under the report's own event id.
    ObservationEnvelope opening = env;
    opening.event_type = "tamper.opened";
    run_rules(opening, r);
    return r;
  }
  run_rules(env, r);
  return r;
}

bool HubEngine::in_maintenance(const DeviceId& device) const {
  auto it = state_.maintenance.find(device);
  return it != state_.maintenance.end() && clock_.now() < it->second.until;
}

void HubEngine::run_rules(const ObservationEnvelope& env, IngestResult& r) {
  const RuleContext ctx{state_.arm_mode, config_.local_minute_of_day
                                             ? config_.local_minute_of_day(clock_.now())
                                             : std::uint16_t{0}};
  const bool suppress = env.event_type == "tamper.opened" && in_maintenance(env.source.device);
  for (const auto& intent : evaluate_rules(config_.rules, config_.property, env, ctx)) {
    if (suppress) {
      const MaintenanceWindow& w = state_.maintenance.at(env.source.device);
      if (Status s = journal(
              AlarmSuppressed{env.source.device, intent.rule, env.event_id, w.until, w.principal});
          !s) {
        r.error = s.error();
        return;
      }
      continue;
    }
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
        return;
      }
    }
    if (req.error) {
      r.error = req.error;
      return;
    }
  }
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
  Authorization auth = evaluate(*principal, normalized, {clock_.now(), f == Freshness::kFresh});
  if (!auth.allowed) return auth;
  // Maintenance windows can't be chained past kMaxMaintenance: a restart while
  // one is open keeps its start, so an open lid alarms at most 4 h later.
  if (const auto* m = std::get_if<SetMaintenance>(&normalized.args)) {
    auto it = state_.maintenance.find(normalized.target.device);
    if (it != state_.maintenance.end() && clock_.now() < it->second.until &&
        clock_.now() + Millis{m->duration_ms} > it->second.since + kMaxMaintenance) {
      return {false, "maintenance_limit"};
    }
  }
  return auth;
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
  if (rec && rec->status == CommandStatus::kAccepted) {
    const OperationSpec* spec = find_operation(rec->command.operation);
    if (spec && spec->hub_internal) {
      if (Status s = execute_internal(command.id); !s) req.error = s.error();
    } else {
      pending_.push_back(command.id);
    }
  }
  return req;
}

Status HubEngine::execute_internal(const CommandId& id) {
  const Timestamp now = clock_.now();
  const Command c = state_.commands.find(id)->command;
  const DeviceId& device = c.target.device;
  std::string evidence = "applied by the hub";
  if (const auto* m = std::get_if<SetMaintenance>(&c.args)) {
    const Timestamp until = now + Millis{m->duration_ms};
    if (Status s = journal(MaintenanceChanged{device, until, c.principal, "started", c.id}); !s) {
      return s;
    }
    evidence = "maintenance until " + std::to_string(until.unix_ms);
  } else if (c.operation == "device.maintenance_end") {
    if (state_.maintenance.contains(device)) {
      if (Status s =
              journal(MaintenanceChanged{device, std::nullopt, c.principal, "cancelled", c.id});
          !s) {
        return s;
      }
      evidence = "maintenance cancelled";
      if (Status s = alarm_if_tamper_open(device); !s) return s;
    } else {
      evidence = "no maintenance to end";
    }
  }
  auto ts = state_.commands.complete_internal(id, now, evidence);
  if (!ts.ok()) return ts.error();
  return journal_all(ts.value());
}

Status HubEngine::alarm_if_tamper_open(const DeviceId& device) {
  const CapabilityRef ref{device, std::nullopt, CapabilityId("tamper.enclosure")};
  auto it = state_.capabilities.find(ref);
  if (it == state_.capabilities.end()) return Status::Ok();
  const auto* t = std::get_if<TamperState>(&it->second.value);
  if (!t || !t->open) return Status::Ok();
  // The lid is still open: evaluate the alarm as for the opening it suppressed.
  // Same event id, so an opening that already alarmed before maintenance began
  // dedupes to the command it made (idempotency key) instead of a second one.
  ObservationEnvelope env;
  env.property = config_.property.id;
  env.source = ref;
  env.event_type = "tamper.opened";
  env.event_id = it->second.last_event;
  env.position = it->second.position;
  env.payload = *t;
  IngestResult r;
  run_rules(env, r);
  if (r.error) return *r.error;
  return Status::Ok();
}

std::vector<Command> HubEngine::hand_off(const DeviceId* device) {
  const Timestamp now = clock_.now();
  std::vector<Command> out;
  std::vector<CommandId> keep;
  for (std::size_t i = 0; i < pending_.size(); ++i) {
    const CommandId& id = pending_[i];
    const CommandRecord* rec = state_.commands.find(id);
    if (!rec || rec->status != CommandStatus::kAccepted) continue;  // expired/canceled
    if (const OperationSpec* spec = find_operation(rec->command.operation);
        spec && spec->hub_internal) {
      continue;  // never a device's to run
    }
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
  std::vector<DeviceId> expired;
  for (const auto& [device, w] : state_.maintenance) {
    if (w.until <= clock_.now()) expired.push_back(device);
  }
  for (const auto& device : expired) {
    if (!s) break;
    s = journal(MaintenanceChanged{device, std::nullopt, kMaintenanceExpiry, "expired", {}});
    if (s) s = alarm_if_tamper_open(device);
  }
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
