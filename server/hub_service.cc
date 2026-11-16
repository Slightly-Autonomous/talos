#include "server/hub_service.h"

#include "codec/codec.h"
#include "core/engine/preview.h"
#include "firmware/bridge/hub_bridge.h"
#include "server/release_signing.h"

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

HubService::HubService(LoadedConfig config, std::unique_ptr<IJournal> journal, const IClock& clock,
                       std::unique_ptr<SqliteConfigStore> config_store,
                       std::unique_ptr<SqliteFirmwareStore> firmware_store)
    : config_(std::move(config)),
      journal_(std::move(journal)),
      clock_(clock),
      store_(std::move(config_store)),
      firmware_(std::move(firmware_store)) {
  EngineConfig ec = config_.engine;
  if (store_) {
    current_ = store_->latest();
    if (!current_) {
      auto boot = store_->append(config_.bootstrap_document, "bootstrap", clock_.now());
      if (boot) current_ = boot.value();
    }
    if (current_) {
      auto parsed = parse_document(current_->document, config_.engine.property.id);
      if (parsed) {
        ec.property = parsed.value().property;
        ec.rules = parsed.value().rules;
        ec.principals = with_rule_principals(config_.base_principals, ec.rules, ec.property.id);
      }
    }
  }
  engine_ = std::make_unique<HubEngine>(ec, *journal_, clock_);
  // Catch up if the store holds a revision the journal has not recorded yet
  // (first start, or a crash between the store write and the journal append).
  if (current_ && engine_->state().config_revision < current_->revision) {
    (void)engine_->apply_config(current_->revision, current_->sha256,
                                PrincipalId(current_->principal), ec.property, ec.rules,
                                ec.principals);
  }
}

talos::v1::Property HubService::property() const {
  std::lock_guard lock(mu_);
  return current_ ? current_->document.property() : config_.property_proto;
}

namespace {
api::ConfigResponse config_response(const ConfigRevision& r) {
  api::ConfigResponse out;
  out.set_revision(r.revision);
  *out.mutable_document() = r.document;
  out.set_created_at_unix_ms(r.created_at.unix_ms);
  out.set_principal_id(r.principal);
  out.set_sha256(r.sha256);
  return out;
}
}  // namespace

Result<api::ConfigResponse> HubService::config(std::optional<Timestamp> at) const {
  std::lock_guard lock(mu_);
  if (!store_ || !current_) return Error{"not_found", "no versioned configuration"};
  if (!at) return config_response(*current_);
  const auto rev = talos::state_at(*journal_, *at).config_revision;
  if (rev == 0) return Error{"not_found", "no configuration revision in force at that time"};
  auto r = store_->get(rev);
  if (!r) return Error{"not_found", "revision " + std::to_string(rev)};
  return config_response(*r);
}

Result<api::ConfigResponse> HubService::put_config(const PrincipalId& who,
                                                   const api::PutConfigRequest& req) {
  std::lock_guard lock(mu_);
  if (!store_ || !current_) return Error{"not_found", "configuration is not editable"};
  const Principal* principal = nullptr;
  for (const auto& p : engine_->config().principals) {
    if (p.id == who) principal = &p;
  }
  if (!principal) return Error{"unknown_principal", who.str()};
  if (auto auth = authorize_config_change(*principal, config_.engine.property.id); !auth.allowed) {
    return Error{"denied", auth.reason};
  }
  if (req.base_revision() != current_->revision) {
    return Error{"conflict", "configuration changed: current revision is " +
                                 std::to_string(current_->revision)};
  }
  auto parsed = parse_document(req.document(), config_.engine.property.id);
  if (!parsed) return parsed.error();
  auto stored = store_->append(req.document(), who.str(), clock_.now());
  if (!stored) return stored.error();
  auto principals = with_rule_principals(config_.base_principals, parsed.value().rules,
                                         parsed.value().property.id);
  if (Status s = engine_->apply_config(stored.value().revision, stored.value().sha256, who,
                                       parsed.value().property, parsed.value().rules, principals);
      !s) {
    return s.error();
  }
  current_ = stored.value();
  after_mutation_locked();
  return config_response(*current_);
}

Result<api::RulePreviewResponse> HubService::preview_rule(
    const api::RulePreviewRequest& req) const {
  auto rule = codec::from_proto(req.rule());
  if (!rule) return rule.error();
  std::lock_guard lock(mu_);
  if (rule.value().property != config_.engine.property.id) {
    return Error{"invalid_request", "rule targets another property"};
  }
  const auto& ec = engine_->config();
  const Timestamp to{req.to_unix_ms() > 0 ? req.to_unix_ms() : clock_.now().unix_ms};
  auto result = talos::preview_rule(rule.value(), ec.property, read_all(*journal_),
                                    Timestamp{req.from_unix_ms()}, to, ec.local_minute_of_day);
  api::RulePreviewResponse out;
  out.set_evaluated_observations(result.evaluated_observations);
  for (const auto& m : result.matches) {
    auto* o = out.add_matches();
    o->set_journal_seq(m.journal_seq);
    o->set_received_at_unix_ms(m.received_at.unix_ms);
    o->set_event_id(m.event_id.str());
    o->set_event_type(m.event_type);
    o->set_device_id(m.device.str());
  }
  return out;
}

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
  return view_locked(engine_->state(), clock_.now());
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
  for (auto& cmd : engine_->take_outbound()) outbound_[cmd.target.device].push_back(std::move(cmd));
  cv_.notify_all();
}

Result<api::SetArmModeResponse> HubService::set_arm_mode(const PrincipalId& who, ArmMode mode) {
  std::lock_guard lock(mu_);
  Status s = engine_->set_arm_mode(mode, who);
  after_mutation_locked();
  if (!s) return s.error();
  api::SetArmModeResponse r;
  r.set_mode(arm_to_proto(engine_->state().arm_mode));
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
  RequestResult rr = engine_->request(c);
  after_mutation_locked();
  const CommandRecord* rec = engine_->state().commands.find(rr.id);
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
  const CommandRecord* rec = engine_->state().commands.find(id);
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
  IngestResult r = engine_->on_observation(env.value());
  after_mutation_locked();
  (r.status == IngestStatus::kAccepted    ? metrics_.ingest_accepted
   : r.status == IngestStatus::kDuplicate ? metrics_.ingest_duplicate
                                          : metrics_.ingest_rejected)++;
  return ingest_response(r);
}

Result<api::IngestResponse> HubService::ingest_ack(const DeviceId& device,
                                                   const talos::v1::CommandAck& p) {
  std::lock_guard lock(mu_);
  const CommandRecord* rec = engine_->state().commands.find(CommandId(p.command_id()));
  if (!rec) return Error{"not_found", "command " + p.command_id()};
  if (rec->command.target.device != device) {
    return Error{"forbidden", "device " + device.str() + " is not the target of " + p.command_id()};
  }
  auto r = engine_->on_ack({CommandId(p.command_id()), p.accepted(), p.detail()});
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
  engine_->tick();
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

// --- Device network plane ---------------------------------------------------------------

namespace {
constexpr std::size_t kAckBytes = 9, kConfigBytes = 17, kOfferBytes = 41, kChunkHeader = 7;
std::uint32_t version_number(const std::string& v) {
  std::uint32_t out = 0;
  for (char ch : v) {
    if (ch < '0' || ch > '9') return 0;
    out = out * 10 + static_cast<std::uint32_t>(ch - '0');
  }
  return out;
}
}  // namespace

std::vector<fw::Downlink> HubService::device_exchange(const DeviceId& device,
                                                      const std::vector<fw::DeviceEvent>& events,
                                                      std::size_t budget) {
  std::lock_guard lock(mu_);
  const Timestamp now = clock_.now();
  std::vector<fw::Downlink> out;
  auto fits = [&](std::size_t n) {
    if (n > budget) return false;
    budget -= n;
    return true;
  };
  for (const auto& e : events) {
    registry_.observe(device, e, now);
    if (auto env = fw::to_envelope(e, device, config_.engine.property.id)) {
      const IngestResult r = engine_->on_observation(*env);
      (r.status == IngestStatus::kAccepted    ? metrics_.ingest_accepted
       : r.status == IngestStatus::kDuplicate ? metrics_.ingest_duplicate
                                              : metrics_.ingest_rejected)++;
    }
    // ACK every authenticated event, even ones ingest rejected: a frame that
    // can never be accepted must not be retried forever.
    if (fits(kAckBytes)) out.push_back(fw::Ack{e.boot_epoch, e.seq});
  }
  after_mutation_locked();

  const DeviceTelemetry* t = registry_.find(device);
  if (auto it = desired_config_.find(device);
      it != desired_config_.end() && (!t || t->applied_config_revision < it->second.revision) &&
      fits(kConfigBytes)) {
    out.push_back(it->second);
  }

  auto camp = campaigns_.find(device);
  if (camp == campaigns_.end() || !firmware_ || !t) return out;
  Campaign& c = camp->second;
  if (c.state == "done" || c.state == "failed" || c.state == "rolled_back") return out;
  const auto ota = t->ota_state.value_or(0);
  if (ota == 0 && t->running_version >= c.version) {
    c.state = "done";
  } else if (ota == 5 && c.state != "scheduled") {
    c.state = "rolled_back";
  } else if (ota == 4) {
    c.state = "failed";
  } else if (ota == 3) {
    c.state = "pending_verify";
  } else if (auto rel = firmware_->get(c.version)) {
    if (ota == 1) {
      c.state = "downloading";
      for (std::uint32_t off = t->ota_received; off < rel->image.size();) {
        fw::OtaChunk ch;
        ch.offset = off;
        ch.length = static_cast<std::uint16_t>(
            std::min<std::size_t>(fw::kOtaChunkMax, rel->image.size() - off));
        if (!fits(kChunkHeader + ch.length)) break;
        std::copy_n(rel->image.data() + off, ch.length, ch.data.begin());
        out.push_back(ch);
        off += ch.length;
        ++metrics_.ota_chunks_sent;
      }
    } else if ((!c.last_offer || now - *c.last_offer > Millis{5'000}) && fits(kOfferBytes)) {
      fw::OtaOffer offer;
      offer.version = c.version;
      offer.size = static_cast<std::uint32_t>(rel->image.size());
      std::copy_n(rel->manifest.image_sha256().data(), 32, offer.sha256.begin());
      out.push_back(offer);
      c.last_offer = now;
      c.state = "offered";
    }
  }
  return out;
}

Status HubService::authorize_owner(const PrincipalId& who) const {
  for (const auto& p : engine_->config().principals) {
    if (p.id != who) continue;
    auto a = authorize_config_change(p, config_.engine.property.id);
    if (!a.allowed) return Error{"denied", a.reason};
    return Status::Ok();
  }
  return Error{"unknown_principal", who.str()};
}

Result<api::DeviceConfigResponse> HubService::set_device_config(
    const PrincipalId& who, const DeviceId& device, const api::DeviceConfigRequest& req) {
  std::lock_guard lock(mu_);
  if (Status s = authorize_owner(who); !s) return s.error();  // can weaken detection
  if (!config_.engine.property.find_device(device)) return Error{"not_found", device.str()};
  fw::ConfigUpdate u;
  // Revision = hub seconds: monotonic across hub restarts.
  u.revision = static_cast<std::uint32_t>(clock_.now().unix_ms / 1000);
  if (auto it = desired_config_.find(device);
      it != desired_config_.end() && u.revision <= it->second.revision) {
    u.revision = it->second.revision + 1;
  }
  u.heartbeat_interval_ms = req.heartbeat_interval_ms();
  u.environment_interval_ms = req.environment_interval_ms();
  u.impact_threshold_mg = req.impact_threshold_mg();
  desired_config_[device] = u;
  api::DeviceConfigResponse r;
  r.set_device_id(device.str());
  r.set_desired_revision(u.revision);
  const auto* t = registry_.find(device);
  r.set_applied_revision(t ? t->applied_config_revision : 0);
  return r;
}

api::HealthListResponse HubService::device_health() const {
  std::lock_guard lock(mu_);
  api::HealthListResponse out;
  const Timestamp now = clock_.now();
  for (const auto& d : config_.engine.property.devices) {
    const CapabilityRef health_ref{d.id, std::nullopt, CapabilityId("device.health")};
    const auto& caps = engine_->state().capabilities;
    std::optional<Timestamp> seen;
    if (auto it = caps.find(health_ref); it != caps.end()) seen = it->second.received_at;
    *out.add_devices() =
        project_health(d.id, registry_.find(d.id), engine_->freshness_of(health_ref), seen, now,
                       engine_->config().freshness_max_age);
  }
  return out;
}

api::DiagnosticsResponse HubService::diagnostics(const DeviceId& device) const {
  std::lock_guard lock(mu_);
  api::DiagnosticsResponse out;
  for (auto& d : registry_.diagnostics(device)) *out.add_diagnostics() = d;
  return out;
}

Status HubService::import_firmware(const PrincipalId& who, const talos::v1::FirmwareRelease& m,
                                   const std::string& image) {
  std::lock_guard lock(mu_);
  if (Status s = authorize_owner(who); !s) return s;
  if (!firmware_) return Error{"not_found", "firmware store not configured"};
  if (version_number(m.version()) == 0)
    return Error{"invalid_release", "version must be a build number > 0"};
  if (Status s = verify_release(m, image, config_.hub.firmware_signing_keys()); !s) return s;
  return firmware_->put(m, image);
}

api::FirmwareReleaseList HubService::firmware_releases() const {
  std::lock_guard lock(mu_);
  api::FirmwareReleaseList out;
  if (firmware_) {
    for (auto& m : firmware_->list()) {
      auto* r = out.add_releases();
      *r = m;
      r->clear_signature();  // listings never need it
    }
  }
  return out;
}

Result<api::CampaignResponse> HubService::start_campaign(const PrincipalId& who,
                                                         const api::CampaignRequest& req) {
  std::lock_guard lock(mu_);
  if (Status s = authorize_owner(who); !s) return s.error();
  const DeviceId device(req.device_id());
  if (!config_.engine.property.find_device(device)) return Error{"not_found", req.device_id()};
  if (!firmware_ || !firmware_->get(req.version())) {
    return Error{"not_found", "release " + std::to_string(req.version())};
  }
  campaigns_[device] = Campaign{req.version(), "scheduled", std::nullopt};
  api::CampaignResponse r;
  r.set_device_id(req.device_id());
  r.set_version(req.version());
  r.set_state("scheduled");
  return r;
}

Result<api::CampaignResponse> HubService::campaign(const DeviceId& device) const {
  std::lock_guard lock(mu_);
  auto it = campaigns_.find(device);
  if (it == campaigns_.end()) return Error{"not_found", "no campaign for " + device.str()};
  api::CampaignResponse r;
  r.set_device_id(device.str());
  r.set_version(it->second.version);
  r.set_state(it->second.state);
  return r;
}

std::string HubService::metrics_text() {
  {
    std::lock_guard lock(mu_);
    metrics_.journal_seq = journal_->last_seq();
  }
  return metrics_.prometheus();
}

}  // namespace talos::server
