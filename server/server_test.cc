#include <google/protobuf/util/json_util.h>
#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "codec/yaml_loader.h"
#include "server/auth.h"
#include "server/config.h"
#include "server/http_server.h"
#include "server/hub_service.h"
#include "server/persistence/config_store.h"
#include "server/persistence/sqlite_journal.h"
#include "server/sha256.h"

namespace talos::server {
namespace {

namespace sv = ::talos::server::v1;
namespace api = ::talos::api::v1;

// --- SHA-256 ------------------------------------------------------------------------------

TEST(Sha256, NistVectors) {
  EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  EXPECT_EQ(sha256_hex(std::string(1'000'000, 'a')),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// --- Config -------------------------------------------------------------------------------

const std::string kOwnerToken = "owner-secret";
const std::string kAgentToken = "agent-secret";
const std::string kNodeToken = "node-07-secret";
const std::string kSpeakerToken = "speaker-secret";
const std::string kNode12Token = "node-12-secret";

sv::HubConfig hub_config(const std::string& sqlite_path) {
  sv::HubConfig h;
  const std::string yaml = R"(
role: HUB_ROLE_HUB
storage: {driver: STORAGE_DRIVER_SQLITE}
sync: {target: SYNC_TARGET_NONE}
auth: AUTH_MODE_LOCAL
property_file: contracts/manifests/properties/sample_backyard.yaml
freshness_max_age_ms: 60000
rules:
  - id: backyard_motion_warning
    revision: 1
    property_id: house-01
    principal_id: "rule:backyard"
    enabled: true
    trigger_event_type: motion.detected
    trigger_zone_id: backyard
    armed_modes: [ARM_MODE_ARMED_AWAY]
    actions:
      - operation: audio.speak
        target: {device_id: patio_speaker, capability_id: audio.speak}
        args: {audio_speak: {text: "This property is monitored.", volume_percent: 65}}
        ttl_ms: 5000
        verification_window_ms: 3000
        expect: {source: {device_id: patio_speaker, capability_id: audio.speak}, payload: {audio_playback: {playing: true}}}
)";
  // Parsed through the YAML loader to exercise the real config path.
  EXPECT_TRUE(codec::parse_yaml(yaml, h));
  h.mutable_storage()->set_sqlite_path(sqlite_path);
  auto add_principal = [&](const std::string& id, sv::PrincipalKind kind,
                           const std::string& token) {
    auto* p = h.add_principals();
    p->set_id(id);
    p->set_kind(kind);
    p->set_max_risk(talos::v1::RISK_CLASS_SECURITY_CRITICAL);
    p->set_token_sha256(sha256_hex(token));
  };
  add_principal("owner", sv::PRINCIPAL_KIND_HUMAN, kOwnerToken);
  add_principal("agent-1", sv::PRINCIPAL_KIND_AGENT, kAgentToken);
  auto add_device = [&](const std::string& id, const std::string& token) {
    auto* d = h.add_devices();
    d->set_device_id(id);
    d->set_token_sha256(sha256_hex(token));
  };
  add_device("node-07", kNodeToken);
  add_device("node-12", kNode12Token);
  add_device("patio_speaker", kSpeakerToken);
  return h;
}

std::string temp_db() {
  static std::atomic<int> n{0};
  const char* dir = std::getenv("TEST_TMPDIR");
  auto p = std::filesystem::path(dir ? dir : "/tmp") / ("hub_" + std::to_string(n++) + ".db");
  std::filesystem::remove(p);
  return p.string();
}

TEST(Config, ValidConfigBuilds) {
  auto c = build_config(hub_config(temp_db()), ".");
  ASSERT_TRUE(c.ok()) << c.error().message;
  EXPECT_EQ(c.value().engine.rules.size(), 1u);
  // Rule principal auto-added.
  EXPECT_EQ(c.value().engine.principals.size(), 3u);
}

TEST(Config, ReservedSwitchesFailLoudly) {
  auto pg = hub_config(temp_db());
  pg.mutable_storage()->set_driver(sv::STORAGE_DRIVER_POSTGRES);
  EXPECT_EQ(build_config(pg, ".").error().code, "reserved_option");
  auto sb = hub_config(temp_db());
  sb.set_auth(sv::AUTH_MODE_SUPABASE);
  EXPECT_EQ(build_config(sb, ".").error().code, "reserved_option");
  auto relay = hub_config(temp_db());
  relay.set_role(sv::HUB_ROLE_RELAY);
  EXPECT_EQ(build_config(relay, ".").error().code, "reserved_option");
}

TEST(Config, SupabaseSyncNeedsEnvNames) {
  auto c = hub_config(temp_db());
  c.mutable_sync()->set_target(sv::SYNC_TARGET_SUPABASE);
  EXPECT_EQ(build_config(c, ".").error().code, "invalid_config");
  c.mutable_sync()->set_supabase_url_env("SUPABASE_URL");
  c.mutable_sync()->set_supabase_service_key_env("SUPABASE_SERVICE_ROLE_KEY");
  EXPECT_TRUE(build_config(c, ".").ok());
}

TEST(Config, RejectsBadCredentials) {
  auto c = hub_config(temp_db());
  c.mutable_principals(0)->set_token_sha256("not-a-hash");
  EXPECT_EQ(build_config(c, ".").error().code, "invalid_config");
  auto d = hub_config(temp_db());
  d.mutable_devices(0)->set_device_id("ghost");
  EXPECT_EQ(build_config(d, ".").error().code, "invalid_config");
}

TEST(Auth, MapsTokensToIdentities) {
  Authenticator a(hub_config(temp_db()));
  auto owner = a.authenticate("Bearer " + kOwnerToken);
  ASSERT_TRUE(owner);
  EXPECT_EQ(owner->kind, Identity::Kind::kPrincipal);
  EXPECT_EQ(owner->id, "owner");
  EXPECT_EQ(a.authenticate("Bearer " + kNodeToken)->kind, Identity::Kind::kDevice);
  EXPECT_FALSE(a.authenticate("Bearer wrong"));
  EXPECT_FALSE(a.authenticate(kOwnerToken));  // missing scheme
  EXPECT_FALSE(a.authenticate(""));
}

// --- HTTP end to end ----------------------------------------------------------------------

class AtomicClock final : public IClock {
 public:
  explicit AtomicClock(std::int64_t t) : t_(t) {}
  Timestamp now() const override { return {t_.load()}; }
  void advance(std::int64_t ms) { t_ += ms; }

 private:
  std::atomic<std::int64_t> t_;
};

std::string json(const google::protobuf::Message& m) {
  std::string out;
  (void)google::protobuf::util::MessageToJsonString(m, &out);
  return out;
}

template <typename M>
M parse(const std::string& body) {
  M m;
  google::protobuf::util::JsonParseOptions o;
  o.ignore_unknown_fields = false;
  EXPECT_TRUE(google::protobuf::util::JsonStringToMessage(body, &m, o).ok()) << body;
  return m;
}

talos::v1::ObservationEnvelope envelope(const std::string& device, std::optional<int> slot,
                                        const std::string& capability, const std::string& type,
                                        std::uint64_t seq, const std::string& payload_json) {
  talos::v1::ObservationEnvelope e;
  e.set_schema_version(1);
  e.set_property_id("house-01");
  e.mutable_source()->set_device_id(device);
  if (slot) e.mutable_source()->set_slot(*slot);
  e.mutable_source()->set_capability_id(capability);
  e.set_event_type(type);
  e.set_event_id(device + ":1:" + std::to_string(seq));
  e.mutable_position()->set_boot_epoch(1);
  e.mutable_position()->set_sequence(seq);
  EXPECT_TRUE(google::protobuf::util::JsonStringToMessage(payload_json, e.mutable_payload()).ok());
  return e;
}

auto motion(std::uint64_t seq) {
  return envelope("node-07", 0, "motion.detect", "motion.detected", seq,
                  R"({"motion":{"detected":true}})");
}

class Hub {
 public:
  Hub(const std::string& db, AtomicClock& clock) {
    auto cfg = build_config(hub_config(db), ".");
    EXPECT_TRUE(cfg.ok()) << (cfg.ok() ? "" : cfg.error().message);
    auth_ = std::make_unique<Authenticator>(cfg.value().hub);
    service_ = std::make_unique<HubService>(cfg.value(), SqliteJournal::open(db).value(), clock,
                                            SqliteConfigStore::open(db).value());
    http_ = std::make_unique<HttpServer>(*service_, *auth_,
                                         std::vector<std::string>{"http://localhost:5173"});
    port_ = http_->bind("127.0.0.1", 0).value();
    thread_ = std::thread([this] { http_->listen(); });
    http_->wait_until_ready();
  }
  ~Hub() {
    http_->stop();
    thread_.join();
  }
  httplib::Client client() const {
    httplib::Client c("127.0.0.1", port_);
    c.set_read_timeout(5, 0);
    return c;
  }
  HubService& service() { return *service_; }

 private:
  std::unique_ptr<Authenticator> auth_;
  std::unique_ptr<HubService> service_;
  std::unique_ptr<HttpServer> http_;
  int port_ = 0;
  std::thread thread_;
};

httplib::Headers bearer(const std::string& token) { return {{"Authorization", "Bearer " + token}}; }

httplib::Result post(httplib::Client& c, const std::string& path, const std::string& token,
                     const std::string& body) {
  return c.Post(path, bearer(token), body, "application/json");
}

class HttpE2E : public ::testing::Test {
 protected:
  std::string db = temp_db();
  AtomicClock clock{1'800'000'000'000};
};

TEST_F(HttpE2E, HealthIsPublicEverythingElseNeedsTheRightCredential) {
  Hub hub(db, clock);
  auto c = hub.client();
  auto h = c.Get("/v1/health");
  ASSERT_TRUE(h);
  EXPECT_EQ(h->status, 200);
  EXPECT_EQ(parse<api::HealthResponse>(h->body).property_id(), "house-01");

  EXPECT_EQ(c.Get("/v1/state")->status, 401);
  EXPECT_EQ(c.Get("/v1/state", bearer("nope"))->status, 401);
  EXPECT_EQ(c.Get("/v1/state", bearer(kNodeToken))->status, 403);  // device on user API
  EXPECT_EQ(c.Get("/v1/state", bearer(kOwnerToken))->status, 200);
  // A person cannot impersonate a device.
  EXPECT_EQ(post(c, "/v1/devices/observations", kOwnerToken, json(motion(1)))->status, 403);
  // A device cannot report for another device.
  auto spoof = envelope("node-12", 0, "contact.open_state", "contact.opened", 1,
                        R"({"contact":{"open":true}})");
  auto r = post(c, "/v1/devices/observations", kNodeToken, json(spoof));
  EXPECT_EQ(r->status, 403);
  EXPECT_EQ(parse<api::ErrorResponse>(r->body).code(), "forbidden");
}

TEST_F(HttpE2E, IntrusionRuleCommandAckAndVerificationOverHttp) {
  Hub hub(db, clock);
  auto c = hub.client();
  ASSERT_EQ(post(c, "/v1/arm", kOwnerToken, R"({"mode":"ARM_MODE_ARMED_AWAY"})")->status, 200);

  auto r = post(c, "/v1/devices/observations", kNodeToken, json(motion(1)));
  ASSERT_EQ(r->status, 200) << r->body;
  EXPECT_EQ(parse<api::IngestResponse>(r->body).status(), "accepted");
  EXPECT_EQ(parse<api::IngestResponse>(
                post(c, "/v1/devices/observations", kNodeToken, json(motion(1)))->body)
                .status(),
            "duplicate");

  // The speaker adapter fetches its commands.
  EXPECT_EQ(parse<api::PendingCommands>(c.Get("/v1/devices/commands", bearer(kNodeToken))->body)
                .commands_size(),
            0);
  auto pending =
      parse<api::PendingCommands>(c.Get("/v1/devices/commands", bearer(kSpeakerToken))->body);
  ASSERT_EQ(pending.commands_size(), 1);
  const std::string id = pending.commands(0).id();
  EXPECT_EQ(pending.commands(0).operation(), "audio.speak");
  // Drained: a second fetch is empty.
  EXPECT_EQ(parse<api::PendingCommands>(c.Get("/v1/devices/commands", bearer(kSpeakerToken))->body)
                .commands_size(),
            0);
  // Only the target device may ACK it.
  talos::v1::CommandAck ack;
  ack.set_command_id(id);
  ack.set_accepted(true);
  EXPECT_EQ(post(c, "/v1/devices/acks", kNodeToken, json(ack))->status, 403);
  clock.advance(300);
  EXPECT_EQ(post(c, "/v1/devices/acks", kSpeakerToken, json(ack))->status, 200);

  auto view = parse<api::CommandView>(c.Get("/v1/commands/" + id, bearer(kOwnerToken))->body);
  EXPECT_EQ(view.status(), talos::v1::COMMAND_STATUS_DEVICE_ACKNOWLEDGED);
  EXPECT_EQ(view.outcome(), api::COMMAND_OUTCOME_PENDING);  // ACK is not verification

  auto playing = envelope("patio_speaker", std::nullopt, "audio.speak", "audio.playback_started", 1,
                          R"({"audioPlayback":{"playing":true}})");
  ASSERT_EQ(post(c, "/v1/devices/observations", kSpeakerToken, json(playing))->status, 200);
  view = parse<api::CommandView>(c.Get("/v1/commands/" + id, bearer(kOwnerToken))->body);
  EXPECT_EQ(view.status(), talos::v1::COMMAND_STATUS_VERIFIED);
  EXPECT_EQ(view.outcome(), api::COMMAND_OUTCOME_VERIFIED);

  auto state = parse<api::StateResponse>(c.Get("/v1/state", bearer(kOwnerToken))->body);
  EXPECT_EQ(state.arm_mode(), talos::v1::ARM_MODE_ARMED_AWAY);
  EXPECT_EQ(state.commands_size(), 1);
}

TEST_F(HttpE2E, SilenceEndsUnknownViaTick) {
  Hub hub(db, clock);
  auto c = hub.client();
  post(c, "/v1/arm", kOwnerToken, R"({"mode":"ARM_MODE_ARMED_AWAY"})");
  post(c, "/v1/devices/observations", kNodeToken, json(motion(1)));
  clock.advance(3'001);
  hub.service().tick();
  auto state = parse<api::StateResponse>(c.Get("/v1/state", bearer(kOwnerToken))->body);
  ASSERT_EQ(state.commands_size(), 1);
  EXPECT_EQ(state.commands(0).outcome(), api::COMMAND_OUTCOME_UNKNOWN);
}

TEST_F(HttpE2E, PolicyDenialsSurfaceAsForbidden) {
  Hub hub(db, clock);
  auto c = hub.client();
  post(c, "/v1/arm", kOwnerToken, R"({"mode":"ARM_MODE_ARMED_AWAY"})");
  auto r = post(c, "/v1/arm", kAgentToken, R"({"mode":"ARM_MODE_DISARMED"})");
  EXPECT_EQ(r->status, 403);
  EXPECT_EQ(parse<api::ErrorResponse>(r->body).message(), "requires_human_authorization");

  // Agent unlock request is recorded and denied (202 + DENIED status).
  auto cmd = post(c, "/v1/commands", kAgentToken, R"({
    "commandId": "agent-unlock", "operation": "access.set_locked",
    "target": {"deviceId": "lock-3", "capabilityId": "access.lock"},
    "args": {"setLocked": {"locked": false}}})");
  ASSERT_EQ(cmd->status, 202) << cmd->body;
  EXPECT_EQ(parse<api::CommandResponse>(cmd->body).status(), talos::v1::COMMAND_STATUS_DENIED);
  EXPECT_EQ(c.Get("/v1/commands/does-not-exist", bearer(kOwnerToken))->status, 404);
  EXPECT_EQ(post(c, "/v1/commands", kOwnerToken, R"({"bogusField": 1})")->status, 400);
}

TEST_F(HttpE2E, JournalPagingAndStateAt) {
  Hub hub(db, clock);
  auto c = hub.client();
  const auto t0 = clock.now().unix_ms;
  clock.advance(1'000);
  post(c, "/v1/arm", kOwnerToken, R"({"mode":"ARM_MODE_ARMED_AWAY"})");
  for (std::uint64_t i = 1; i <= 4; ++i) {
    clock.advance(100);
    post(c, "/v1/devices/observations", kNode12Token,
         json(envelope("node-12", 0, "contact.open_state",
                       i % 2 ? "contact.opened" : "contact.closed", i,
                       i % 2 ? R"({"contact":{"open":true}})" : R"({"contact":{"open":false}})")));
  }
  std::uint64_t after = 0;
  int entries = 0, pages = 0;
  while (true) {
    auto page = parse<api::JournalPage>(
        c.Get("/v1/journal?after=" + std::to_string(after) + "&limit=2", bearer(kOwnerToken))
            ->body);
    entries += page.entries_size();
    ++pages;
    after = page.next_after();
    if (!page.has_more()) break;
  }
  EXPECT_EQ(entries, 6);  // bootstrap config revision + arm + 4 contacts
  EXPECT_EQ(pages, 3);

  auto before = parse<api::StateResponse>(
      c.Get("/v1/state_at?t=" + std::to_string(t0), bearer(kOwnerToken))->body);
  EXPECT_EQ(before.arm_mode(), talos::v1::ARM_MODE_DISARMED);
  EXPECT_EQ(before.capabilities_size(), 0);
}

TEST_F(HttpE2E, RestartRebuildsHistoryFromSqlite) {
  api::StateResponse first;
  {
    Hub hub(db, clock);
    auto c = hub.client();
    post(c, "/v1/arm", kOwnerToken, R"({"mode":"ARM_MODE_ARMED_AWAY"})");
    post(c, "/v1/devices/observations", kNodeToken, json(motion(1)));
    first = parse<api::StateResponse>(c.Get("/v1/state", bearer(kOwnerToken))->body);
  }
  Hub restarted(db, clock);
  auto c = restarted.client();
  auto second = parse<api::StateResponse>(c.Get("/v1/state", bearer(kOwnerToken))->body);
  EXPECT_EQ(second.through_seq(), first.through_seq());
  EXPECT_EQ(second.arm_mode(), talos::v1::ARM_MODE_ARMED_AWAY);
  EXPECT_EQ(second.commands_size(), 1);
  EXPECT_EQ(parse<api::IngestResponse>(
                post(c, "/v1/devices/observations", kNodeToken, json(motion(1)))->body)
                .status(),
            "duplicate");
}

TEST_F(HttpE2E, EventStreamDeliversNewJournalEntries) {
  Hub hub(db, clock);
  std::string received;
  std::thread writer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto c = hub.client();
    post(c, "/v1/arm", kOwnerToken, R"({"mode":"ARM_MODE_ARMED_AWAY"})");
  });
  auto c = hub.client();
  // Entry 1 is the bootstrap config revision; stream what comes after it.
  auto res = c.Get("/v1/events?after=1", bearer(kOwnerToken), [&](const char* data, std::size_t n) {
    received.append(data, n);
    return received.find("\n\n") == std::string::npos;  // stop after one event
  });
  writer.join();
  EXPECT_NE(received.find("event: journal"), std::string::npos) << received;
  EXPECT_NE(received.find("armModeChanged"), std::string::npos) << received;
}

TEST_F(HttpE2E, CorsOnlyForConfiguredOrigins) {
  Hub hub(db, clock);
  auto c = hub.client();
  auto ok = c.Get("/v1/health", {{"Origin", "http://localhost:5173"}});
  EXPECT_EQ(ok->get_header_value("Access-Control-Allow-Origin"), "http://localhost:5173");
  auto evil = c.Get("/v1/health", {{"Origin", "https://evil.example"}});
  EXPECT_FALSE(evil->has_header("Access-Control-Allow-Origin"));
}

// --- Versioned configuration (ADR 0012) ------------------------------------------------

api::ConfigResponse get_config(httplib::Client& c, const std::string& query = "") {
  auto r = c.Get("/v1/config" + query, bearer(kOwnerToken));
  EXPECT_EQ(r->status, 200) << r->body;
  return parse<api::ConfigResponse>(r->body);
}

httplib::Result put_config(httplib::Client& c, const std::string& token, std::uint32_t base,
                           const sv::ConfigDocument& doc) {
  api::PutConfigRequest req;
  req.set_base_revision(base);
  *req.mutable_document() = doc;
  return c.Put("/v1/config", bearer(token), json(req), "application/json");
}

TEST_F(HttpE2E, ConfigIsVersionedAuthorizedAndHistoryKeepsOldLayout) {
  Hub hub(db, clock);
  auto c = hub.client();
  auto v1 = get_config(c);
  EXPECT_EQ(v1.revision(), 1u);
  EXPECT_EQ(v1.principal_id(), "bootstrap");
  const auto t_v1 = clock.now().unix_ms;

  clock.advance(1'000);
  sv::ConfigDocument doc = v1.document();
  for (auto& z : *doc.mutable_property()->mutable_zones()) {
    if (z.id() == "patio") z.set_name("Back patio");
  }
  auto r = put_config(c, kOwnerToken, 1, doc);
  ASSERT_EQ(r->status, 200) << r->body;
  EXPECT_EQ(parse<api::ConfigResponse>(r->body).revision(), 2u);

  // Current property reflects the edit; history still sees revision 1.
  auto prop = parse<talos::v1::Property>(c.Get("/v1/property", bearer(kOwnerToken))->body);
  bool renamed = false;
  for (const auto& z : prop.zones()) renamed |= z.name() == "Back patio";
  EXPECT_TRUE(renamed);
  EXPECT_EQ(get_config(c, "?at=" + std::to_string(t_v1)).revision(), 1u);

  // Stale base revision -> 409; agents -> 403; invalid documents -> 400.
  EXPECT_EQ(put_config(c, kOwnerToken, 1, doc)->status, 409);
  auto agent = put_config(c, kAgentToken, 2, doc);
  EXPECT_EQ(agent->status, 403);
  EXPECT_EQ(parse<api::ErrorResponse>(agent->body).message(), "requires_human_authorization");
  sv::ConfigDocument bad = doc;
  bad.mutable_rules(0)->mutable_actions(0)->mutable_target()->set_device_id("ghost");
  auto invalid = put_config(c, kOwnerToken, 2, bad);
  EXPECT_EQ(invalid->status, 400);
  EXPECT_EQ(parse<api::ErrorResponse>(invalid->body).code(), "invalid_config");

  // The change is journaled.
  auto page = parse<api::JournalPage>(c.Get("/v1/journal?after=0", bearer(kOwnerToken))->body);
  int revisions = 0;
  for (const auto& e : page.entries()) revisions += e.has_config_revision_applied();
  EXPECT_EQ(revisions, 2);
}

TEST_F(HttpE2E, RemovingTheRuleStopsTheWarningAndSurvivesRestart) {
  {
    Hub hub(db, clock);
    auto c = hub.client();
    post(c, "/v1/arm", kOwnerToken, R"({"mode":"ARM_MODE_ARMED_AWAY"})");
    sv::ConfigDocument doc = get_config(c).document();
    doc.clear_rules();
    ASSERT_EQ(put_config(c, kOwnerToken, 1, doc)->status, 200);
    post(c, "/v1/devices/observations", kNodeToken, json(motion(1)));
    EXPECT_EQ(
        parse<api::StateResponse>(c.Get("/v1/state", bearer(kOwnerToken))->body).commands_size(),
        0);
  }
  Hub restarted(db, clock);
  auto c = restarted.client();
  EXPECT_EQ(get_config(c).revision(), 2u);
  EXPECT_EQ(get_config(c).document().rules_size(), 0);
  post(c, "/v1/devices/observations", kNodeToken, json(motion(2)));
  auto state = parse<api::StateResponse>(c.Get("/v1/state", bearer(kOwnerToken))->body);
  EXPECT_EQ(state.commands_size(), 0);
  // No duplicate revision events after restart.
  auto page = parse<api::JournalPage>(c.Get("/v1/journal?after=0", bearer(kOwnerToken))->body);
  int revisions = 0;
  for (const auto& e : page.entries()) revisions += e.has_config_revision_applied();
  EXPECT_EQ(revisions, 2);
}

TEST_F(HttpE2E, RulePreviewDryRunsAgainstHistory) {
  Hub hub(db, clock);
  auto c = hub.client();
  post(c, "/v1/arm", kOwnerToken, R"({"mode":"ARM_MODE_ARMED_AWAY"})");
  post(c, "/v1/devices/observations", kNodeToken, json(motion(1)));
  const auto commands_before =
      parse<api::StateResponse>(c.Get("/v1/state", bearer(kOwnerToken))->body).commands_size();

  api::RulePreviewRequest req;
  *req.mutable_rule() = get_config(c).document().rules(0);
  req.set_from_unix_ms(0);
  auto r = c.Post("/v1/rules/preview", bearer(kOwnerToken), json(req), "application/json");
  ASSERT_EQ(r->status, 200) << r->body;
  auto preview = parse<api::RulePreviewResponse>(r->body);
  ASSERT_EQ(preview.matches_size(), 1);
  EXPECT_EQ(preview.matches(0).event_id(), "node-07:1:1");
  // Dry run: nothing new dispatched.
  EXPECT_EQ(
      parse<api::StateResponse>(c.Get("/v1/state", bearer(kOwnerToken))->body).commands_size(),
      commands_before);
}

}  // namespace
}  // namespace talos::server
