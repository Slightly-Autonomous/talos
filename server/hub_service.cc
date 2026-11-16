#include "server/hub_service.h"

#include "codec/codec.h"

namespace talos::server {

namespace api = ::talos::api::v1;

namespace {

api::Freshness to_api(Freshness f) {
  switch (f) {
    case Freshness::kFresh:
      return api::FRESHNESS_FRESH;
    case Freshness::kStale:
      return api::FRESHNESS_STALE;
    case Freshness::kUnknown:
      return api::FRESHNESS_UNKNOWN;
  }
  return api::FRESHNESS_UNSPECIFIED;
}

api::CommandOutcome to_api(CommandOutcome o) {
  switch (o) {
    case CommandOutcome::kPending:
      return api::COMMAND_OUTCOME_PENDING;
    case CommandOutcome::kVerified:
      return api::COMMAND_OUTCOME_VERIFIED;
    case CommandOutcome::kUnverified:
      return api::COMMAND_OUTCOME_UNVERIFIED;
    case CommandOutcome::kFailed:
      return api::COMMAND_OUTCOME_FAILED;
    case CommandOutcome::kUnknown:
      return api::COMMAND_OUTCOME_UNKNOWN;
  }
  return api::COMMAND_OUTCOME_UNSPECIFIED;
}

talos::v1::ArmMode arm_to_proto(ArmMode m) {
  switch (m) {
    case ArmMode::kDisarmed:
      return talos::v1::ARM_MODE_DISARMED;
    case ArmMode::kArmedHome:
      return talos::v1::ARM_MODE_ARMED_HOME;
    case ArmMode::kArmedAway:
      return talos::v1::ARM_MODE_ARMED_AWAY;
  }
  return talos::v1::ARM_MODE_UNSPECIFIED;
}

talos::v1::CommandStatus status_to_proto(CommandStatus s) {
  CommandTransition t;
  t.to = s;
  return codec::to_proto(t).to();
}

api::CommandView command_view(const CommandRecord& rec) {
  api::CommandView v;
  *v.mutable_command() = codec::to_proto(rec.command);
  v.set_status(status_to_proto(rec.status));
  v.set_outcome(to_api(outcome_of(rec.status)));
  if (rec.dispatched_at) v.set_dispatched_at_unix_ms(rec.dispatched_at->unix_ms);
  for (const auto& t : rec.history) *v.add_history() = codec::to_proto(t);
  return v;
}

api::IngestResponse ingest_response(const IngestResult& r) {
  api::IngestResponse out;
  switch (r.status) {
    case IngestStatus::kAccepted:
      out.set_status("accepted");
      break;
    case IngestStatus::kDuplicate:
      out.set_status("duplicate");
      break;
    case IngestStatus::kTooOld:
      out.set_status("too_old");
      break;
    case IngestStatus::kRejected:
      out.set_status("rejected");
      break;
  }
  if (r.entry) out.set_journal_seq(r.entry->journal_seq);
  if (r.error) {
    out.set_error_code(r.error->code);
    out.set_error_message(r.error->message);
  }
  return out;
}

}  // namespace

HubService::HubService(LoadedConfig config, std::unique_ptr<IJournal> journal, const IClock& clock)
    : config_(std::move(config)),
      journal_(std::move(journal)),
      clock_(clock),
      engine_(config_.engine, *journal_, clock_) {}

api::HealthResponse HubService::health() const {
  std::lock_guard lock(mu_);
  api::HealthResponse h;
  h.set_status("ok");
  h.set_version("0.3.0");
  h.set_role("hub");
  h.set_journal_seq(journal_->last_seq());
  h.set_property_id(config_.engine.property.id.str());
  return h;
}

api::StateResponse HubService::view_locked(const PropertyState& s, Timestamp now) const {
  api::StateResponse out;
  out.set_property_id(config_.engine.property.id.str());
  out.set_through_seq(s.through_seq);
  out.set_arm_mode(arm_to_proto(s.arm_mode));
  out.set_server_time_unix_ms(now.unix_ms);
  for (const auto& [ref, cap] : s.capabilities) {
    auto* v = out.add_capabilities();
    ObservationEnvelope shim;  // reuse codec for ref/payload mapping
    shim.source = ref;
    shim.payload = cap.value;
    auto p = codec::to_proto(shim);
    *v->mutable_ref() = p.source();
    *v->mutable_value() = p.payload();
    v->set_event_type(cap.event_type);
    v->set_last_event_id(cap.last_event.str());
    v->set_received_at_unix_ms(cap.received_at.unix_ms);
    if (cap.observed_at) v->set_observed_at_unix_ms(cap.observed_at->unix_ms);
    v->set_freshness(to_api(freshness(s, ref, now, config_.engine.freshness_max_age)));
    v->set_journal_seq(cap.journal_seq);
  }
  for (const auto& [id, rec] : s.commands.records()) *out.add_commands() = command_view(rec);
  return out;
}

api::StateResponse HubService::state() const {
  std::lock_guard lock(mu_);
  return view_locked(engine_.state(), clock_.now());
}

api::StateResponse HubService::state_at(Timestamp t) const {
  std::lock_guard lock(mu_);
  return view_locked(talos::state_at(*journal_, t), t);
}

api::JournalPage HubService::journal_page(std::uint64_t after, std::uint32_t limit) const {
  std::lock_guard lock(mu_);
  const std::uint32_t n = limit == 0 || limit > kMaxPage ? kMaxPage : limit;
  auto entries = journal_->read(after, n + 1);
  api::JournalPage page;
  page.set_has_more(entries.size() > n);
  if (entries.size() > n) entries.resize(n);
  for (const auto& e : entries) *page.add_entries() = codec::to_proto(e);
  page.set_next_after(entries.empty() ? after : entries.back().journal_seq);
  return page;
}

void HubService::after_mutation_locked() {
  for (auto& cmd : engine_.take_outbound()) outbound_[cmd.target.device].push_back(std::move(cmd));
  cv_.notify_all();
}

Result<api::SetArmModeResponse> HubService::set_arm_mode(const PrincipalId& who, ArmMode mode) {
  std::lock_guard lock(mu_);
  Status s = engine_.set_arm_mode(mode, who);
  after_mutation_locked();
  if (!s) return s.error();
  api::SetArmModeResponse r;
  r.set_mode(arm_to_proto(engine_.state().arm_mode));
  return r;
}

Result<api::CommandResponse> HubService::request_command(const PrincipalId& who,
                                                         const api::CommandRequest& req) {
  if (req.command_id().empty()) return Error{"invalid_request", "command_id is required"};
  auto target = codec::from_proto(req.target());
  if (!target) return target.error();
  auto args = codec::from_proto(req.args());
  if (!args) return args.error();

  std::lock_guard lock(mu_);
  const Timestamp now = clock_.now();
  Command c;
  c.id = CommandId(req.command_id());
  c.idempotency_key = req.idempotency_key().empty() ? req.command_id() : req.idempotency_key();
  c.principal = who;
  c.property = config_.engine.property.id;
  c.target = target.value();
  c.operation = req.operation();
  c.args = args.value();
  c.created_at = now;
  c.expires_at = now + Millis{req.ttl_ms() > 0 ? req.ttl_ms() : 5'000};
  c.verification_window =
      Millis{req.verification_window_ms() > 0 ? req.verification_window_ms() : 5'000};
  if (req.has_expect()) {
    auto src = codec::from_proto(req.expect().source());
    if (!src) return src.error();
    c.expect = ExpectedObservation{src.value(), codec::from_proto(req.expect().payload())};
    c.verification = VerificationMode::kObservedState;
  } else {
    c.verification = VerificationMode::kAckOnly;
  }
  RequestResult rr = engine_.request(c);
  after_mutation_locked();
  const CommandRecord* rec = engine_.state().commands.find(rr.id);
  api::CommandResponse out;
  out.set_command_id(rr.id.str());
  out.set_duplicate(rr.duplicate);
  if (rec) {
    out.set_status(status_to_proto(rec->status));
    out.set_reason(rec->history.empty() ? "" : rec->history.back().reason);
  }
  return out;
}

Result<api::CommandView> HubService::command(const CommandId& id) const {
  std::lock_guard lock(mu_);
  const CommandRecord* rec = engine_.state().commands.find(id);
  if (!rec) return Error{"not_found", "command " + id.str()};
  return command_view(*rec);
}

Result<api::IngestResponse> HubService::ingest_observation(
    const DeviceId& device, const talos::v1::ObservationEnvelope& p) {
  auto env = codec::from_proto(p);
  if (!env) return env.error();
  if (env.value().source.device != device) {
    return Error{"forbidden", "device " + device.str() + " cannot report for " +
                                  env.value().source.device.str()};
  }
  std::lock_guard lock(mu_);
  IngestResult r = engine_.on_observation(env.value());
  after_mutation_locked();
  return ingest_response(r);
}

Result<api::IngestResponse> HubService::ingest_ack(const DeviceId& device,
                                                   const talos::v1::CommandAck& p) {
  std::lock_guard lock(mu_);
  const CommandRecord* rec = engine_.state().commands.find(CommandId(p.command_id()));
  if (!rec) return Error{"not_found", "command " + p.command_id()};
  if (rec->command.target.device != device) {
    return Error{"forbidden", "device " + device.str() + " is not the target of " + p.command_id()};
  }
  auto r = engine_.on_ack({CommandId(p.command_id()), p.accepted(), p.detail()});
  after_mutation_locked();
  if (!r) return r.error();
  api::IngestResponse out;
  out.set_status("accepted");
  return out;
}

api::PendingCommands HubService::take_commands(const DeviceId& device) {
  std::lock_guard lock(mu_);
  api::PendingCommands out;
  auto it = outbound_.find(device);
  if (it == outbound_.end()) return out;
  for (const auto& c : it->second) *out.add_commands() = codec::to_proto(c);
  it->second.clear();
  return out;
}

void HubService::tick() {
  std::lock_guard lock(mu_);
  const auto before = journal_->last_seq();
  engine_.tick();
  if (journal_->last_seq() != before) after_mutation_locked();
}

std::vector<JournalEntry> HubService::wait_for_entries(std::uint64_t after,
                                                       std::chrono::milliseconds timeout,
                                                       std::size_t limit) {
  std::unique_lock lock(mu_);
  cv_.wait_for(lock, timeout, [&] { return shutting_down_ || journal_->last_seq() > after; });
  return journal_->read(after, limit);
}

void HubService::shutdown() {
  std::lock_guard lock(mu_);
  shutting_down_ = true;
  cv_.notify_all();
}

}  // namespace talos::server
