#include <arpa/inet.h>
#include <google/protobuf/util/json_util.h>
#include <gtest/gtest.h>
#include <httplib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#include "codec/yaml_loader.h"
#include "common/sha256/sha256.h"
#include "firmware/bridge/hub_bridge.h"
#include "firmware/portable/hal.h"
#include "server/auth.h"
#include "server/config.h"
#include "server/http_server.h"
#include "server/hub_service.h"
#include "server/jwt.h"
#include "server/persistence/config_store.h"
#include "server/persistence/sqlite_journal.h"

#ifdef MSG_NOSIGNAL
#define MSG_NOSIGNAL_COMPAT MSG_NOSIGNAL
#else
#define MSG_NOSIGNAL_COMPAT 0
#endif

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
const std::string kLockToken = "lock-3-secret";
const std::string kMetricsToken = "metrics-secret";

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
  auto add_principal = [&](const std::string& id, sv::PrincipalKind kind, const std::string& token,
                           talos::v1::RiskClass risk = talos::v1::RISK_CLASS_SECURITY_CRITICAL) {
    auto* p = h.add_principals();
    p->set_id(id);
    p->set_kind(kind);
    p->set_max_risk(risk);
    p->set_token_sha256(sha256_hex(token));
  };
  add_principal("owner", sv::PRINCIPAL_KIND_HUMAN, kOwnerToken);
  add_principal("agent-1", sv::PRINCIPAL_KIND_AGENT, kAgentToken);
  add_principal("metrics", sv::PRINCIPAL_KIND_SYSTEM, kMetricsToken,
                talos::v1::RISK_CLASS_INFORMATIONAL);
  auto add_device = [&](const std::string& id, const std::string& token) {
    auto* d = h.add_devices();
    d->set_device_id(id);
    d->set_token_sha256(sha256_hex(token));
  };
  add_device("node-07", kNodeToken);
  add_device("node-12", kNode12Token);
  add_device("patio_speaker", kSpeakerToken);
  add_device("lock-3", kLockToken);
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
  EXPECT_EQ(c.value().engine.principals.size(), 4u);
}

TEST(Config, ReservedSwitchesFailLoudly) {
  auto pg = hub_config(temp_db());
  pg.mutable_storage()->set_driver(sv::STORAGE_DRIVER_POSTGRES);
  EXPECT_EQ(build_config(pg, ".").error().code, "reserved_option");
  auto sb = hub_config(temp_db());
  sb.set_auth(sv::AUTH_MODE_SUPABASE);  // now implemented, but needs a secret env name
  EXPECT_EQ(build_config(sb, ".").error().code, "invalid_config");
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

TEST(Config, RulePrincipalsAreAlwaysRulesNeverConfiguredIdentities) {
  // A rule naming the owner would run with the owner's security-critical grant.
  auto c = hub_config(temp_db());
  c.mutable_rules(0)->set_principal_id("owner");
  auto r = build_config(c, ".");
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, "invalid_config");
  // Distinct rule ids get one capped kRule principal each.
  auto ok = build_config(hub_config(temp_db()), ".");
  ASSERT_TRUE(ok.ok());
  for (const auto& p : ok.value().engine.principals) {
    if (p.id == PrincipalId("rule:backyard")) {
      EXPECT_EQ(p.kind, PrincipalKind::kRule);
      EXPECT_EQ(p.grants.at(0).max_risk, RiskClass::kDisruptiveOutput);
    }
  }
}

TEST(Config, TokenHashesAreUniqueAcrossPrincipalsAndDevices) {
  auto c = hub_config(temp_db());
  c.mutable_devices(0)->set_token_sha256(sha256_hex(kOwnerToken));
  EXPECT_EQ(build_config(c, ".").error().code, "invalid_config");
}

TEST(Config, RuleActionsMustMatchTheOperationCatalog) {
  auto c = hub_config(temp_db());
  // Speak operation, unlock args: refused when the config loads.
  auto* args = c.mutable_rules(0)->mutable_actions(0)->mutable_args();
  args->mutable_set_locked()->set_locked(false);
  EXPECT_EQ(build_config(c, ".").error().code, "invalid_config");
  auto e = hub_config(temp_db());
  e.mutable_rules(0)->mutable_actions(0)->mutable_expect()->mutable_payload()->mutable_lock();
  EXPECT_EQ(build_config(e, ".").error().code, "invalid_config");
}

TEST(Config, DevModeRefusesNetworkHostsForHttpAndCoap) {
  auto c = hub_config(temp_db());
  c.set_dev_mode(true);
  EXPECT_TRUE(check_dev_mode_hosts(c, "127.0.0.1", false));
  EXPECT_FALSE(check_dev_mode_hosts(c, "0.0.0.0", false));
  c.mutable_coap()->set_enabled(true);
  c.mutable_coap()->set_listen_host("0.0.0.0");  // HTTP on loopback, CoAP exposed
  EXPECT_FALSE(check_dev_mode_hosts(c, "127.0.0.1", false));
  EXPECT_TRUE(check_dev_mode_hosts(c, "127.0.0.1", true));  // explicit override
  c.set_dev_mode(false);
  EXPECT_TRUE(check_dev_mode_hosts(c, "0.0.0.0", false));
}

// Red team #16: published dev credentials are refused on the network even
// without dev_mode.
TEST(Config, PublishedDevCredentialsAreRefusedOnTheNetworkWithoutDevMode) {
  auto c = hub_config(temp_db());
  ASSERT_FALSE(c.dev_mode());
  EXPECT_TRUE(check_dev_mode_hosts(c, "0.0.0.0", false));
  auto dev_owner = c;  // hub.dev.yaml's owner hash
  dev_owner.mutable_principals(0)->set_token_sha256(
      "8ac4afa44f3f01da2d47daaee53270af09cdf9b0482481377b750332904ddd94");
  EXPECT_EQ(check_dev_mode_hosts(dev_owner, "0.0.0.0", false).error().code, "dev_mode_on_network");
  EXPECT_TRUE(check_dev_mode_hosts(dev_owner, "127.0.0.1", false));
  auto dev_device = c;
  dev_device.mutable_devices(0)->set_token_sha256(
      "3a0c053a94f5d03270628729fd2f84ba52065698f1b95719409e93042df778a3");
  EXPECT_FALSE(check_dev_mode_hosts(dev_device, "192.168.1.10", false));
  // dev-device-keys.yaml's node-07 key, with CoAP on the network.
  c.mutable_coap()->set_enabled(true);
  c.mutable_coap()->set_listen_host("0.0.0.0");
  std::vector<std::uint8_t> dev_key;
  for (int i = 0; i < 16; ++i) dev_key.push_back(static_cast<std::uint8_t>(i));
  EXPECT_FALSE(check_dev_mode_hosts(c, "127.0.0.1", false, {dev_key}));
  EXPECT_TRUE(check_dev_mode_hosts(c, "127.0.0.1", false, {std::vector<std::uint8_t>(16, 0x5a)}));
}

TEST(HttpStatus, HubFailuresAreServerErrors) {
  EXPECT_EQ(http_status_for(Error{"storage_error", ""}), 500);
  EXPECT_EQ(http_status_for(Error{"corrupt_journal", ""}), 500);
  EXPECT_EQ(http_status_for(Error{"invalid_request", ""}), 400);
  EXPECT_EQ(http_status_for(Error{"stale_revision", ""}), 409);
  EXPECT_EQ(http_status_for(Error{"denied", ""}), 403);
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
  Hub(const std::string& db, AtomicClock& clock,
      int max_event_streams = HttpServer::kMaxEventStreams,
      HttpServer::ConnectionLimits limits = {}) {
    auto cfg = build_config(hub_config(db), ".");
    EXPECT_TRUE(cfg.ok()) << (cfg.ok() ? "" : cfg.error().message);
    auth_ = std::make_unique<Authenticator>(cfg.value().hub);
    service_ = std::make_unique<HubService>(cfg.value(), SqliteJournal::open(db).value(), clock,
                                            SqliteConfigStore::open(db).value());
    http_ = std::make_unique<HttpServer>(*service_, *auth_,
                                         std::vector<std::string>{"http://localhost:5173"},
                                         max_event_streams, limits);
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
  // The speaker adapter takes the command (the dispatch), then stays silent.
  ASSERT_EQ(parse<api::PendingCommands>(c.Get("/v1/devices/commands", bearer(kSpeakerToken))->body)
                .commands_size(),
            1);
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

// --- Command safety over the real API (review B1, B2, B3, m1, m2) -------------------------

auto lock_state(std::uint64_t seq, bool locked) {
  return envelope("lock-3", std::nullopt, "access.lock", "lock.state", seq,
                  locked ? R"({"lock":{"locked":true}})" : R"({"lock":{"locked":false}})");
}

int lock_queue_size(httplib::Client& c) {
  return parse<api::PendingCommands>(c.Get("/v1/devices/commands", bearer(kLockToken))->body)
      .commands_size();
}

api::CommandResponse command_response(const httplib::Result& r) {
  EXPECT_EQ(r->status, 202) << r->body;
  return parse<api::CommandResponse>(r->body);
}

TEST_F(HttpE2E, AgentCannotSmuggleUnlockArgsUnderALowRiskOperation) {
  Hub hub(db, clock);
  auto c = hub.client();
  ASSERT_EQ(post(c, "/v1/devices/observations", kLockToken, json(lock_state(1, true)))->status,
            200);
  auto r = command_response(post(c, "/v1/commands", kAgentToken, R"({
    "commandId": "smuggle", "operation": "audio.speak",
    "target": {"deviceId": "lock-3", "capabilityId": "access.lock"},
    "args": {"setLocked": {"locked": false}}})"));
  EXPECT_EQ(r.status(), talos::v1::COMMAND_STATUS_DENIED);
  EXPECT_EQ(r.reason(), "args_do_not_match_operation");
  EXPECT_EQ(lock_queue_size(c), 0);
}

TEST_F(HttpE2E, ExpiredCommandIsNeverHandedToTheDevice) {
  Hub hub(db, clock);
  auto c = hub.client();
  post(c, "/v1/devices/observations", kLockToken, json(lock_state(1, true)));
  auto r = command_response(post(c, "/v1/commands", kOwnerToken, R"({
    "commandId": "late1", "operation": "access.set_locked", "ttlMs": 100,
    "verificationWindowMs": 200,
    "target": {"deviceId": "lock-3", "capabilityId": "access.lock"},
    "args": {"setLocked": {"locked": false}}})"));
  EXPECT_EQ(r.status(), talos::v1::COMMAND_STATUS_ACCEPTED);  // waits for hand-off
  clock.advance(2'000);                                       // the lock adapter was offline
  hub.service().tick();
  EXPECT_EQ(lock_queue_size(c), 0);
  auto view = parse<api::CommandView>(c.Get("/v1/commands/late1", bearer(kOwnerToken))->body);
  EXPECT_EQ(view.status(), talos::v1::COMMAND_STATUS_EXPIRED);
  // Even without a tick in between, a poll after the TTL hands nothing over.
  post(c, "/v1/devices/observations", kLockToken, json(lock_state(2, true)));
  auto r2 = command_response(post(c, "/v1/commands", kOwnerToken, R"({
    "commandId": "late2", "operation": "access.set_locked", "ttlMs": 100,
    "target": {"deviceId": "lock-3", "capabilityId": "access.lock"},
    "args": {"setLocked": {"locked": false}}})"));
  ASSERT_EQ(r2.status(), talos::v1::COMMAND_STATUS_ACCEPTED);
  clock.advance(200);
  EXPECT_EQ(lock_queue_size(c), 0);
  view = parse<api::CommandView>(c.Get("/v1/commands/late2", bearer(kOwnerToken))->body);
  EXPECT_EQ(view.status(), talos::v1::COMMAND_STATUS_EXPIRED);
}

TEST_F(HttpE2E, CallerCannotChooseWhatIsFreshOrWhatVerifies) {
  Hub hub(db, clock);
  auto c = hub.client();
  auto beat = envelope("lock-3", std::nullopt, "device.health", "device.heartbeat", 1,
                       R"({"heartbeat":{"batteryMv":3000}})");
  ASSERT_EQ(post(c, "/v1/devices/observations", kLockToken, json(beat))->status, 200);
  // Lock state was never reported. Pointing expect at the heartbeat used to
  // pass the freshness gate and let the next heartbeat "verify" the unlock.
  auto r = command_response(post(c, "/v1/commands", kOwnerToken, R"({
    "commandId": "bypass", "operation": "access.set_locked",
    "target": {"deviceId": "lock-3", "capabilityId": "access.lock"},
    "args": {"setLocked": {"locked": false}},
    "expect": {"source": {"deviceId": "lock-3", "capabilityId": "device.health"},
               "payload": {"heartbeat": {"batteryMv": 3000}}}})"));
  EXPECT_EQ(r.status(), talos::v1::COMMAND_STATUS_DENIED);
  EXPECT_EQ(r.reason(), "expectation_does_not_match_operation");
  EXPECT_EQ(lock_queue_size(c), 0);

  // An honest unlock without expect gets the derived expectation; only lock
  // state, not a heartbeat, verifies it.
  post(c, "/v1/devices/observations", kLockToken, json(lock_state(2, true)));
  auto ok = command_response(post(c, "/v1/commands", kOwnerToken, R"({
    "commandId": "honest", "operation": "access.set_locked",
    "target": {"deviceId": "lock-3", "capabilityId": "access.lock"},
    "args": {"setLocked": {"locked": false}}})"));
  ASSERT_EQ(ok.status(), talos::v1::COMMAND_STATUS_ACCEPTED);
  ASSERT_EQ(lock_queue_size(c), 1);
  auto beat2 = envelope("lock-3", std::nullopt, "device.health", "device.heartbeat", 3,
                        R"({"heartbeat":{"batteryMv":3000}})");
  post(c, "/v1/devices/observations", kLockToken, json(beat2));
  auto view = parse<api::CommandView>(c.Get("/v1/commands/honest", bearer(kOwnerToken))->body);
  EXPECT_EQ(view.status(), talos::v1::COMMAND_STATUS_DISPATCHED);
  EXPECT_EQ(view.command().expect().source().capability_id(), "access.lock");
  post(c, "/v1/devices/observations", kLockToken, json(lock_state(4, false)));
  view = parse<api::CommandView>(c.Get("/v1/commands/honest", bearer(kOwnerToken))->body);
  EXPECT_EQ(view.status(), talos::v1::COMMAND_STATUS_VERIFIED);
}

TEST_F(HttpE2E, RequestFieldsAreBounded) {
  Hub hub(db, clock);
  auto c = hub.client();
  post(c, "/v1/devices/observations", kLockToken, json(lock_state(1, true)));
  auto ttl = command_response(post(c, "/v1/commands", kOwnerToken, R"({
    "commandId": "long", "operation": "access.set_locked", "ttlMs": 4000000000,
    "target": {"deviceId": "lock-3", "capabilityId": "access.lock"},
    "args": {"setLocked": {"locked": true}}})"));
  EXPECT_EQ(ttl.reason(), "ttl_out_of_bounds");
  auto ghost = command_response(post(c, "/v1/commands", kOwnerToken, R"({
    "commandId": "ghost", "operation": "audio.speak",
    "target": {"deviceId": "ghost", "capabilityId": "audio.speak"},
    "args": {"audioSpeak": {"text": "hi"}}})"));
  EXPECT_EQ(ghost.reason(), "unknown_target_device");
  auto bad_id = post(c, "/v1/commands", kOwnerToken, R"({
    "commandId": "has spaces/and slashes", "operation": "audio.speak",
    "target": {"deviceId": "patio_speaker", "capabilityId": "audio.speak"},
    "args": {"audioSpeak": {"text": "hi"}}})");
  EXPECT_EQ(bad_id->status, 400);
  EXPECT_EQ(post(c, "/v1/commands", kOwnerToken,
                 R"({"commandId": ")" + std::string(200, 'a') + R"(", "operation": "notify"})")
                ->status,
            400);
}

TEST_F(HttpE2E, IdempotencyKeysAndCommandIdsBelongToOnePrincipal) {
  Hub hub(db, clock);
  auto c = hub.client();
  const std::string speak = R"("operation": "audio.speak",
    "target": {"deviceId": "patio_speaker", "capabilityId": "audio.speak"},
    "args": {"audioSpeak": {"text": "hi"}})";
  auto owner =
      command_response(post(c, "/v1/commands", kOwnerToken,
                            R"({"commandId": "x4", "idempotencyKey": "k", )" + speak + "}"));
  EXPECT_FALSE(owner.duplicate());
  // The agent's key "k" is its own: a new command, not the owner's.
  auto agent =
      command_response(post(c, "/v1/commands", kAgentToken,
                            R"({"commandId": "a1", "idempotencyKey": "k", )" + speak + "}"));
  EXPECT_FALSE(agent.duplicate());
  EXPECT_EQ(agent.command_id(), "a1");
  // Reusing the owner's command id is a conflict that reveals nothing.
  auto steal = post(c, "/v1/commands", kAgentToken, R"({"commandId": "x4", )" + speak + "}");
  EXPECT_EQ(steal->status, 409);
  // The owner retrying gets its own command back.
  auto retry =
      command_response(post(c, "/v1/commands", kOwnerToken,
                            R"({"commandId": "x5", "idempotencyKey": "k", )" + speak + "}"));
  EXPECT_TRUE(retry.duplicate());
  EXPECT_EQ(retry.command_id(), "x4");
}

TEST_F(HttpE2E, RulePrincipalCollidingWithAPersonIsRejectedOnPut) {
  Hub hub(db, clock);
  auto c = hub.client();
  sv::ConfigDocument doc = get_config(c).document();
  doc.mutable_rules(0)->set_principal_id("owner");
  auto r = put_config(c, kOwnerToken, 1, doc);
  EXPECT_EQ(r->status, 400);
  EXPECT_EQ(parse<api::ErrorResponse>(r->body).code(), "invalid_config");
  EXPECT_EQ(get_config(c).revision(), 1u);
}

TEST_F(HttpE2E, DevicesAddedByAConfigEditAppearInHealthAndAcceptConfig) {
  Hub hub(db, clock);
  auto c = hub.client();
  sv::ConfigDocument doc = get_config(c).document();
  auto* d = doc.mutable_property()->add_devices();
  d->set_id("node-99");
  d->set_property_id("house-01");
  ASSERT_EQ(put_config(c, kOwnerToken, 1, doc)->status, 200);
  auto health =
      parse<api::HealthListResponse>(c.Get("/v1/health/devices", bearer(kOwnerToken))->body);
  bool listed = false;
  for (const auto& h : health.devices()) listed |= h.device_id() == "node-99";
  EXPECT_TRUE(listed);
  EXPECT_EQ(post(c, "/v1/devices/node-99/config", kOwnerToken, R"({"heartbeatIntervalMs": 60000})")
                ->status,
            200);
}

TEST_F(HttpE2E, ReplayedHeartbeatDoesNotKeepADeadDeviceAlive) {
  Hub hub(db, clock);
  fw::DeviceEvent beat;
  beat.boot_epoch = 1;
  beat.seq = 1;
  beat.kind = fw::EventKind::kHeartbeat;
  beat.a = 3'000;
  (void)hub.service().device_exchange(DeviceId("node-12"), {beat}, 512);
  const auto first_seen = clock.now().unix_ms;
  clock.advance(20 * 60 * 1000);  // silent well past the offline threshold
  (void)hub.service().device_exchange(DeviceId("node-12"), {beat}, 512);  // captured frame
  fw::DeviceEvent ota;
  ota.boot_epoch = 1;
  ota.seq = 2;
  ota.kind = fw::EventKind::kOtaStatus;
  ota.a = 0;
  ota.b = 7;
  (void)hub.service().device_exchange(DeviceId("node-12"), {ota}, 512);
  clock.advance(20 * 60 * 1000);
  (void)hub.service().device_exchange(DeviceId("node-12"), {ota}, 512);  // replayed OTA status
  const auto health = hub.service().device_health();
  for (const auto& h : health.devices()) {
    if (h.device_id() != "node-12") continue;
    EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_OFFLINE);
    EXPECT_EQ(h.last_seen_unix_ms(), first_seen + 20 * 60 * 1000);  // the genuine OTA report
  }
}

// Red team: maintenance covers the tamper alarm only. It used to return
// MAINTENANCE before looking at anything else, hiding a slot fault or a flat
// battery on that node. Offline still wins and still names an open lid.
TEST(ProjectHealth, MaintenanceCoversOnlyTheTamperAlarm) {
  const DeviceId dev("node-12");
  const Timestamp now{1'000'000};
  const Millis offline{60'000};
  const auto until = std::optional<Timestamp>(now + Millis{600'000});
  DeviceTelemetry t;
  t.last_seen = now;
  auto h = project_health(dev, &t, Freshness::kFresh, now, now, offline, nullptr, true, until);
  EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_MAINTENANCE);
  t.slots[1] = {0x22, fw::ModuleFault::kBusError};
  h = project_health(dev, &t, Freshness::kFresh, now, now, offline, nullptr, true, until);
  EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_FAULTED);
  bool noted = false;
  for (const auto& e : h.evidence()) noted |= e.find("maintenance") != std::string::npos;
  EXPECT_TRUE(noted);
  t.slots.clear();
  t.power_mode = fw::PowerMode::kCritical;
  h = project_health(dev, &t, Freshness::kFresh, now, now, offline, nullptr, false, until);
  EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_LOW_POWER);
  t.power_mode = fw::PowerMode::kNormal;
  t.last_seen = now - Millis{120'000};
  h = project_health(dev, &t, Freshness::kStale, std::nullopt, now, offline, nullptr, true, until);
  EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_OFFLINE);
  bool lid = false;
  for (const auto& e : h.evidence()) lid |= e.find("enclosure") != std::string::npos;
  EXPECT_TRUE(lid);
}

// Red team: the registry copy of a node's tamper (nodes that don't declare
// tamper.enclosure) took whatever arrived last, so a late "closed" cleared an
// open lid. Only the newest source position counts.
TEST(DeviceRegistryTamper, ALateClosedReportDoesNotClearAnOpenLid) {
  DeviceRegistry reg;
  fw::DeviceEvent open;
  open.kind = fw::EventKind::kTamper;
  open.boot_epoch = 1;
  open.seq = 5;
  open.flag = true;
  fw::DeviceEvent closed = open;
  closed.seq = 4;
  closed.flag = false;
  ASSERT_TRUE(reg.observe_unjournaled(DeviceId("n"), open, Timestamp{1}));
  ASSERT_TRUE(reg.observe_unjournaled(DeviceId("n"), closed, Timestamp{2}));
  EXPECT_EQ(reg.find(DeviceId("n"))->tamper_open, std::optional<bool>{true});
  closed.seq = 6;
  ASSERT_TRUE(reg.observe_unjournaled(DeviceId("n"), closed, Timestamp{3}));
  EXPECT_EQ(reg.find(DeviceId("n"))->tamper_open, std::optional<bool>{false});
}

// HD-032: a node's enclosure tamper report shows in its health until the lid closes.
TEST_F(HttpE2E, TamperOpenFaultsTheDeviceUntilRestored) {
  Hub hub(db, clock);
  auto health_of = [&](const std::string& id) {
    const auto all = hub.service().device_health();
    for (const auto& h : all.devices()) {
      if (h.device_id() == id) return h;
    }
    return talos::v1::Health{};
  };
  fw::DeviceEvent beat;
  beat.boot_epoch = 1;
  beat.seq = 1;
  beat.kind = fw::EventKind::kHeartbeat;
  beat.a = 3'900;
  (void)hub.service().device_exchange(DeviceId("node-12"), {beat}, 512);
  EXPECT_EQ(health_of("node-12").status(), talos::v1::HEALTH_STATUS_HEALTHY);
  fw::DeviceEvent open;
  open.boot_epoch = 1;
  open.seq = 2;
  open.kind = fw::EventKind::kTamper;
  open.priority = fw::Priority::kP0Critical;
  open.flag = true;
  const auto acks = hub.service().device_exchange(DeviceId("node-12"), {open}, 512);
  ASSERT_FALSE(acks.empty());
  EXPECT_TRUE(std::holds_alternative<fw::Ack>(acks.front()));
  const auto h = health_of("node-12");
  EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_FAULTED);
  // node-12 declares tamper.enclosure: the opening is journaled as an observation.
  auto c = hub.client();
  auto journal = parse<api::JournalPage>(c.Get("/v1/journal", bearer(kOwnerToken))->body);
  bool journaled = false;
  for (const auto& e : journal.entries()) {
    journaled |= e.has_observation() &&
                 e.observation().envelope().event_type() == "tamper.opened" &&
                 e.observation().envelope().source().capability_id() == "tamper.enclosure";
  }
  EXPECT_TRUE(journaled) << journal.DebugString();
  ASSERT_GT(h.evidence_size(), 0);
  EXPECT_NE(h.evidence(0).find("tamper"), std::string::npos);
  fw::DeviceEvent closed = open;
  closed.seq = 3;
  closed.priority = fw::Priority::kP1State;
  closed.flag = false;
  (void)hub.service().device_exchange(DeviceId("node-12"), {closed}, 512);
  EXPECT_EQ(health_of("node-12").status(), talos::v1::HEALTH_STATUS_HEALTHY);
}

// The HTTP device API (devsim, adapters) reaches the same alarm path: journaled,
// and health shows the open enclosure even with a fresh heartbeat. Another
// device cannot report node-12's tamper.
TEST_F(HttpE2E, TamperOverTheDeviceApiIsJournaledAndFaultsHealth) {
  Hub hub(db, clock);
  auto c = hub.client();
  auto health_of = [&](const std::string& id) {
    auto h = parse<api::HealthListResponse>(c.Get("/v1/health/devices", bearer(kOwnerToken))->body);
    for (const auto& d : h.devices()) {
      if (d.device_id() == id) return d.status();
    }
    return talos::v1::HEALTH_STATUS_UNSPECIFIED;
  };
  auto beat = envelope("node-12", std::nullopt, "device.health", "device.heartbeat", 1,
                       R"({"heartbeat":{"batteryMv":3900}})");
  ASSERT_EQ(post(c, "/v1/devices/observations", kNode12Token, json(beat))->status, 200);
  EXPECT_EQ(health_of("node-12"), talos::v1::HEALTH_STATUS_HEALTHY);
  auto open = envelope("node-12", std::nullopt, "tamper.enclosure", "tamper.opened", 2,
                       R"({"tamper":{"open":true}})");
  auto spoof = open;
  spoof.mutable_source()->set_device_id("node-07");  // node-07's token, node-07 has no lid loop
  spoof.set_event_id("node-07:1:9");
  EXPECT_NE(post(c, "/v1/devices/observations", kNodeToken, json(spoof))->status, 200);
  ASSERT_EQ(post(c, "/v1/devices/observations", kNode12Token, json(open))->status, 200);
  EXPECT_EQ(health_of("node-12"), talos::v1::HEALTH_STATUS_FAULTED);
  auto closed = envelope("node-12", std::nullopt, "tamper.enclosure", "tamper.closed", 3,
                         R"({"tamper":{"open":false}})");
  ASSERT_EQ(post(c, "/v1/devices/observations", kNode12Token, json(closed))->status, 200);
  EXPECT_EQ(health_of("node-12"), talos::v1::HEALTH_STATUS_HEALTHY);
}

// Maintenance over the ordinary command API: humans only, journaled, shown in
// state and health, and it expires on the hub clock.
TEST_F(HttpE2E, MaintenanceIsAHumanCommandShownInStateAndHealthUntilItExpires) {
  Hub hub(db, clock);
  auto c = hub.client();
  auto beat = envelope("node-12", std::nullopt, "device.health", "device.heartbeat", 1,
                       R"({"heartbeat":{"batteryMv":3900}})");
  ASSERT_EQ(post(c, "/v1/devices/observations", kNode12Token, json(beat))->status, 200);
  const std::string body = R"({"commandId": "%ID%", "operation": "device.maintenance",
    "target": {"deviceId": "node-12", "capabilityId": "device.health"},
    "args": {"setMaintenance": {"durationMs": 600000}}})";
  auto with_id = [&](const std::string& id) {
    std::string b = body;
    return b.replace(b.find("%ID%"), 4, id);
  };
  auto agent = post(c, "/v1/commands", kAgentToken, with_id("agent-maint"));
  ASSERT_EQ(agent->status, 202) << agent->body;
  EXPECT_EQ(parse<api::CommandResponse>(agent->body).status(), talos::v1::COMMAND_STATUS_DENIED);
  auto owner = post(c, "/v1/commands", kOwnerToken, with_id("owner-maint"));
  ASSERT_EQ(owner->status, 202) << owner->body;
  EXPECT_EQ(parse<api::CommandResponse>(owner->body).status(), talos::v1::COMMAND_STATUS_VERIFIED);
  auto state = parse<api::StateResponse>(c.Get("/v1/state", bearer(kOwnerToken))->body);
  ASSERT_EQ(state.maintenance_size(), 1);
  EXPECT_EQ(state.maintenance(0).device_id(), "node-12");
  EXPECT_EQ(state.maintenance(0).until_unix_ms(), clock.now().unix_ms + 600'000);
  EXPECT_EQ(state.maintenance(0).principal_id(), "owner");
  auto health_of = [&](const std::string& id) {
    auto h = parse<api::HealthListResponse>(c.Get("/v1/health/devices", bearer(kOwnerToken))->body);
    for (const auto& d : h.devices()) {
      if (d.device_id() == id) return d.status();
    }
    return talos::v1::HEALTH_STATUS_UNSPECIFIED;
  };
  EXPECT_EQ(health_of("node-12"), talos::v1::HEALTH_STATUS_MAINTENANCE);
  clock.advance(600'000);
  hub.service().tick();
  state = parse<api::StateResponse>(c.Get("/v1/state", bearer(kOwnerToken))->body);
  EXPECT_EQ(state.maintenance_size(), 0);
  auto journal = parse<api::JournalPage>(c.Get("/v1/journal", bearer(kOwnerToken))->body);
  int started = 0, expired = 0;
  for (const auto& e : journal.entries()) {
    if (!e.has_maintenance_changed()) continue;
    started += e.maintenance_changed().reason() == "started";
    expired += e.maintenance_changed().reason() == "expired";
  }
  EXPECT_EQ(started, 1);
  EXPECT_EQ(expired, 1);
}

TEST_F(HttpE2E, LiveEventStreamsAreCapped) {
  constexpr int kCap = 2;
  Hub hub(db, clock, kCap);
  std::atomic<bool> stop{false};
  std::vector<std::thread> streams;
  std::atomic<int> open{0};
  for (int i = 0; i < kCap; ++i) {
    streams.emplace_back([&] {
      auto c = hub.client();
      c.set_read_timeout(30, 0);
      bool counted = false;
      c.Get(
          "/v1/events", bearer(kOwnerToken),
          [&](const httplib::Response&) {
            if (!counted) ++open;
            counted = true;
            return true;
          },
          [&](const char*, std::size_t) { return !stop.load(); });
    });
  }
  for (int i = 0; i < 1000 && open.load() < kCap; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(open.load(), kCap);
  auto c = hub.client();
  auto extra = c.Get("/v1/events", bearer(kOwnerToken));
  ASSERT_TRUE(extra);
  EXPECT_EQ(extra->status, 503);
  // Ordinary requests still get a worker.
  EXPECT_EQ(c.Get("/v1/state", bearer(kOwnerToken))->status, 200);
  stop = true;
  // Wake the streams so their receivers see `stop`.
  post(c, "/v1/arm", kOwnerToken, R"({"mode":"ARM_MODE_ARMED_HOME"})");
  for (auto& t : streams) t.join();
}

// --- Red team (2026-11-20), ported from scratchpad/redteam ------------------------------

std::uint32_t lock_pending(httplib::Client& c) {
  return static_cast<std::uint32_t>(lock_queue_size(c));
}

// #1 (a11_late_verify.py): a delayed, older "locked" report must not verify
// the lock command while the newer state says unlocked.
TEST_F(HttpE2E, StaleObservationNeverVerifiesALockCommand) {
  Hub hub(db, clock);
  auto c = hub.client();
  ASSERT_EQ(post(c, "/v1/devices/observations", kLockToken, json(lock_state(6, false)))->status,
            200);
  auto r = command_response(post(c, "/v1/commands", kOwnerToken, R"({
    "commandId": "own-lock-1", "operation": "access.set_locked",
    "target": {"deviceId": "lock-3", "capabilityId": "access.lock"},
    "args": {"setLocked": {"locked": true}}})"));
  ASSERT_EQ(r.status(), talos::v1::COMMAND_STATUS_ACCEPTED);
  ASSERT_EQ(lock_pending(c), 1u);  // dispatched
  clock.advance(100);
  ASSERT_EQ(post(c, "/v1/devices/observations", kLockToken, json(lock_state(4, true)))->status,
            200);
  auto view = parse<api::CommandView>(c.Get("/v1/commands/own-lock-1", bearer(kOwnerToken))->body);
  EXPECT_NE(view.status(), talos::v1::COMMAND_STATUS_VERIFIED);
  EXPECT_NE(view.outcome(), api::COMMAND_OUTCOME_VERIFIED);
}

// #2 (a2_preclaim.py): a low-privilege principal cannot pre-claim the
// predictable ids of rule commands; the rule still acts.
TEST_F(HttpE2E, ForeignPrincipalCannotPreclaimRuleCommandIds) {
  Hub hub(db, clock);
  auto c = hub.client();
  ASSERT_EQ(post(c, "/v1/arm", kOwnerToken, R"({"mode":"ARM_MODE_ARMED_AWAY"})")->status, 200);
  for (int seq = 1; seq <= 5; ++seq) {
    auto squat = post(c, "/v1/commands", kMetricsToken,
                      R"({"commandId": "cmd:backyard_motion_warning@1:node-07:1:)" +
                          std::to_string(seq) + R"(:0", "operation": "state.read"})");
    EXPECT_EQ(squat->status, 400) << squat->body;  // the rule namespace is reserved
  }
  // A denied request under any other id never holds that id either.
  command_response(post(c, "/v1/commands", kMetricsToken, R"({
    "commandId": "shared-id", "operation": "audio.speak",
    "target": {"deviceId": "patio_speaker", "capabilityId": "audio.speak"},
    "args": {"audioSpeak": {"text": "hi"}}})"));
  auto owner = command_response(post(c, "/v1/commands", kOwnerToken, R"({
    "commandId": "shared-id", "operation": "audio.speak",
    "target": {"deviceId": "patio_speaker", "capabilityId": "audio.speak"},
    "args": {"audioSpeak": {"text": "hi"}}})"));
  EXPECT_EQ(owner.status(), talos::v1::COMMAND_STATUS_ACCEPTED);

  ASSERT_EQ(post(c, "/v1/devices/observations", kNodeToken, json(motion(3)))->status, 200);
  auto state = parse<api::StateResponse>(c.Get("/v1/state", bearer(kOwnerToken))->body);
  bool rule_command = false;
  for (const auto& v : state.commands()) {
    rule_command |= v.command().principal_id() == "rule:backyard" &&
                    v.command().causation_id() == "node-07:1:3";
  }
  EXPECT_TRUE(rule_command);
}

// #8 (a7_spam.py): command requests are rate limited per principal, and
// denials stop being journaled once the principal's audit budget is spent.
TEST_F(HttpE2E, CommandFloodIsRateLimitedAndDenialsAreBounded) {
  Hub hub(db, clock);
  auto c = hub.client();
  const auto before = hub.service().health().journal_seq();
  int limited = 0;
  for (int i = 0; i < 200; ++i) {
    auto r = post(c, "/v1/commands", kMetricsToken,
                  R"({"commandId": "spam-)" + std::to_string(i) +
                      R"(", "operation": "audio.speak",
    "target": {"deviceId": "patio_speaker", "capabilityId": "audio.speak"},
    "args": {"audioSpeak": {"text": "hi"}}})");
    if (r->status == 429) ++limited;
    if (i % 10 == 9) clock.advance(1'000);  // 10 requests per second, sustained
  }
  EXPECT_GT(limited, 0);
  // At most the audit burst plus what refilled was journaled (2 entries each).
  const auto journaled = hub.service().health().journal_seq() - before;
  EXPECT_LE(journaled, 2u * (HubService::kDenialAuditBurst + 10));
  // The owner is unaffected by another principal's budget.
  post(c, "/v1/devices/observations", kLockToken, json(lock_state(1, true)));
  auto ok = command_response(post(c, "/v1/commands", kOwnerToken, R"({
    "commandId": "owner-ok", "operation": "access.set_locked",
    "target": {"deviceId": "lock-3", "capabilityId": "access.lock"},
    "args": {"setLocked": {"locked": true}}})"));
  EXPECT_EQ(ok.status(), talos::v1::COMMAND_STATUS_ACCEPTED);
}

// #7: an agent cannot step ARMED_AWAY down to ARMED_HOME over the API.
TEST_F(HttpE2E, AgentCannotLowerTheArmMode) {
  Hub hub(db, clock);
  auto c = hub.client();
  ASSERT_EQ(post(c, "/v1/arm", kOwnerToken, R"({"mode":"ARM_MODE_ARMED_AWAY"})")->status, 200);
  auto r = post(c, "/v1/arm", kAgentToken, R"({"mode":"ARM_MODE_ARMED_HOME"})");
  EXPECT_EQ(r->status, 403);
  EXPECT_EQ(parse<api::StateResponse>(c.Get("/v1/state", bearer(kOwnerToken))->body).arm_mode(),
            talos::v1::ARM_MODE_ARMED_AWAY);
}

// #12: device text is bounded, event ids are namespaced by the device, and
// event_type must agree with the payload.
TEST_F(HttpE2E, ObservationFieldsAreBoundedAndConsistent) {
  Hub hub(db, clock);
  auto c = hub.client();
  auto lie = lock_state(1, true);
  lie.set_event_type("lock.unlocked");
  auto r = post(c, "/v1/devices/observations", kLockToken, json(lie));
  EXPECT_EQ(r->status, 422);
  EXPECT_EQ(parse<api::IngestResponse>(r->body).error_code(), "event_type_does_not_match_payload");
  auto huge = lock_state(2, true);
  huge.set_event_id("lock-3:" + std::string(30'000, 'a'));
  EXPECT_EQ(post(c, "/v1/devices/observations", kLockToken, json(huge))->status, 422);
  auto foreign_id = lock_state(3, true);
  foreign_id.set_event_id("node-07:1:3");
  EXPECT_EQ(post(c, "/v1/devices/observations", kLockToken, json(foreign_id))->status, 400);
  talos::v1::CommandAck ack;
  ack.set_command_id("nope");
  ack.set_detail(std::string(1'000, 'x'));
  EXPECT_EQ(post(c, "/v1/devices/acks", kLockToken, json(ack))->status, 400);
}

// #4 (a8_epoch.py): a huge boot epoch is refused; a device whose real reports
// land behind its record is not "healthy"; the owner can reset it.
TEST_F(HttpE2E, PoisonedBootEpochIsRefusedSurfacedAndRecoverable) {
  Hub hub(db, clock);
  auto c = hub.client();
  auto poison = motion(1);
  poison.mutable_position()->set_boot_epoch(0xFFFF'FFFFu);
  poison.mutable_position()->set_sequence(0xFFFF'FFFF'FFFF'FFFFull);
  auto r = post(c, "/v1/devices/observations", kNodeToken, json(poison));
  EXPECT_EQ(r->status, 422);
  EXPECT_EQ(parse<api::IngestResponse>(r->body).error_code(), "source_position_out_of_range");
  auto health_of = [&](const std::string& id) {
    auto h = parse<api::HealthListResponse>(c.Get("/v1/health/devices", bearer(kOwnerToken))->body);
    for (const auto& d : h.devices()) {
      if (d.device_id() == id) return d.status();
    }
    return talos::v1::HEALTH_STATUS_UNSPECIFIED;
  };
  auto beat = envelope("node-07", std::nullopt, "device.health", "device.heartbeat", 2,
                       R"({"heartbeat":{"batteryMv":3900}})");
  ASSERT_EQ(post(c, "/v1/devices/observations", kNodeToken, json(beat))->status, 200);
  EXPECT_EQ(health_of("node-07"), talos::v1::HEALTH_STATUS_FAULTED);
  // Owner only.
  EXPECT_EQ(post(c, "/v1/devices/node-07/sequence/reset", kAgentToken, "")->status, 403);
  EXPECT_EQ(post(c, "/v1/devices/node-07/sequence/reset", kOwnerToken, "")->status, 200);
  EXPECT_EQ(health_of("node-07"), talos::v1::HEALTH_STATUS_HEALTHY);
  auto journal = parse<api::JournalPage>(c.Get("/v1/journal", bearer(kOwnerToken))->body);
  EXPECT_TRUE(journal.entries(journal.entries_size() - 1).has_source_reset());
}

// #11 (a6b.py): a low-privilege principal cannot take every live stream.
TEST_F(HttpE2E, LowPrivilegePrincipalCannotTakeTheOwnersStreams) {
  constexpr int kCap = 6;  // 3 reserved for owners
  Hub hub(db, clock, kCap);
  std::atomic<bool> stop{false};
  std::atomic<int> open{0};
  std::vector<std::thread> streams;
  auto stream = [&](const std::string& token) {
    streams.emplace_back([&, token] {
      auto c = hub.client();
      c.set_read_timeout(30, 0);
      bool counted = false;
      c.Get(
          "/v1/events", bearer(token),
          [&](const httplib::Response& res) {
            if (!counted && res.status == 200) ++open;
            counted = true;
            return res.status == 200;
          },
          [&](const char*, std::size_t) { return !stop.load(); });
    });
  };
  for (int i = 0; i < 3; ++i) stream(kAgentToken);
  for (int i = 0; i < 1000 && open.load() < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(open.load(), 3);
  auto c = hub.client();
  EXPECT_EQ(c.Get("/v1/events", bearer(kAgentToken))->status, 503);  // non-owner share used
  stream(kOwnerToken);
  for (int i = 0; i < 1000 && open.load() < 4; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(open.load(), 4);  // the owner still gets a stream
  stop = true;
  post(c, "/v1/arm", kOwnerToken, R"({"mode":"ARM_MODE_ARMED_HOME"})");
  for (auto& t : streams) t.join();
}

// #9 (a5_slowloris.py): connections trickling headers are cut at the read
// deadline, and one peer cannot hold more than its share of connections.
TEST_F(HttpE2E, SlowlorisConnectionsAreCutAndCapped) {
  Hub hub(db, clock, HttpServer::kMaxEventStreams,
          {/*read_deadline_seconds=*/1,
           /*max_connections_per_peer=*/4});
  std::signal(SIGPIPE, SIG_IGN);  // writes to sockets the hub cut
  const int port = hub.client().port();
  auto slow = [&] {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<std::uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
    const char* start = "GET /v1/state HTTP/1.1\r\nHost: x\r\n";
    EXPECT_GT(::send(fd, start, std::strlen(start), 0), 0);
    return fd;
  };
  std::vector<int> fds;
  for (int i = 0; i < 4; ++i) fds.push_back(slow());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  // A fifth connection from the same peer is refused at once.
  {
    auto c = hub.client();
    auto r = c.Get("/v1/state", bearer(kOwnerToken));
    EXPECT_FALSE(r && r->status == 200);
  }
  // Keep trickling header bytes well past the deadline: the hub cuts them.
  bool all_closed = false;
  for (int t = 0; t < 40 && !all_closed; ++t) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    all_closed = true;
    for (int fd : fds) {
      (void)::send(fd, "X", 1, MSG_NOSIGNAL_COMPAT);
      char b;
      const auto n = ::recv(fd, &b, 1, MSG_DONTWAIT);
      all_closed &= n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
    }
  }
  EXPECT_TRUE(all_closed);
  for (int fd : fds) ::close(fd);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(hub.client().Get("/v1/state", bearer(kOwnerToken))->status, 200);
}

// --- Supabase JWT auth (ADR 0015) ---------------------------------------------------------

std::string make_jwt(const std::string& header, const std::string& payload,
                     const std::string& secret) {
  const std::string input = base64url_encode(header) + "." + base64url_encode(payload);
  HmacSha256 mac(reinterpret_cast<const std::uint8_t*>(secret.data()), secret.size());
  mac.update(reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
  const auto sig = mac.finish();
  return input + "." +
         base64url_encode(std::string(reinterpret_cast<const char*>(sig.data()), sig.size()));
}

TEST(SupabaseAuth, AcceptsMappedValidTokensOnly) {
  auto h = hub_config(temp_db());
  h.set_auth(sv::AUTH_MODE_SUPABASE);
  h.mutable_supabase_auth()->set_jwt_secret_env("SUPABASE_JWT_SECRET");
  auto* u = h.mutable_supabase_auth()->add_users();
  u->set_user_id("6f1c-uuid");
  u->set_principal_id("owner");
  ASSERT_TRUE(build_config(h, ".").ok());
  auto bad_map = h;
  bad_map.mutable_supabase_auth()->mutable_users(0)->set_principal_id("nobody");
  EXPECT_EQ(build_config(bad_map, ".").error().code, "invalid_config");

  AtomicClock clock(1'800'000'000'000);
  const std::string secret = "super-secret-jwt-key";
  Authenticator a(h, secret, &clock);
  const std::string hs = R"({"alg":"HS256","typ":"JWT"})";
  auto payload = [](const std::string& sub, std::int64_t exp, const std::string& aud) {
    return R"({"sub":")" + sub + R"(","exp":)" + std::to_string(exp) + R"(,"aud":")" + aud +
           R"(","role":"authenticated"})";
  };
  const std::int64_t now = 1'800'000'000;
  auto ok = a.authenticate("Bearer " +
                           make_jwt(hs, payload("6f1c-uuid", now + 600, "authenticated"), secret));
  ASSERT_TRUE(ok);
  EXPECT_EQ(ok->id, "owner");
  EXPECT_FALSE(a.authenticate(
      "Bearer " + make_jwt(hs, payload("6f1c-uuid", now - 1, "authenticated"), secret)));
  EXPECT_FALSE(a.authenticate(
      "Bearer " + make_jwt(hs, payload("6f1c-uuid", now + 600, "authenticated"), "wrong")));
  EXPECT_FALSE(a.authenticate(
      "Bearer " + make_jwt(hs, payload("stranger", now + 600, "authenticated"), secret)));
  EXPECT_FALSE(
      a.authenticate("Bearer " + make_jwt(hs, payload("6f1c-uuid", now + 600, "anon"), secret)));
  // alg "none" with an empty signature is never accepted.
  const std::string none = base64url_encode(R"({"alg":"none"})") + "." +
                           base64url_encode(payload("6f1c-uuid", now + 600, "authenticated")) + ".";
  EXPECT_FALSE(a.authenticate("Bearer " + none));
  // Local tokens keep working in Supabase mode.
  EXPECT_EQ(a.authenticate("Bearer " + kOwnerToken)->id, "owner");
}

TEST(SupabaseAuth, OutOfRangeTimeClaimsIssuerAndDottedLocalTokens) {
  auto h = hub_config(temp_db());
  h.set_auth(sv::AUTH_MODE_SUPABASE);
  h.mutable_supabase_auth()->set_jwt_secret_env("SUPABASE_JWT_SECRET");
  h.mutable_supabase_auth()->set_issuer("https://example.supabase.co/auth/v1");
  auto* u = h.mutable_supabase_auth()->add_users();
  u->set_user_id("6f1c-uuid");
  u->set_principal_id("owner");
  // A local token that happens to have two dots.
  h.mutable_principals(1)->set_token_sha256(sha256_hex("agent.token.v1"));
  AtomicClock clock(1'800'000'000'000);
  const std::string secret = "super-secret-jwt-key";
  Authenticator a(h, secret, &clock);
  const std::string hs = R"({"alg":"HS256","typ":"JWT"})";
  auto token = [&](const std::string& exp, const std::string& iss) {
    return "Bearer " + make_jwt(hs,
                                R"({"sub":"6f1c-uuid","aud":"authenticated","exp":)" + exp +
                                    R"(,"iss":")" + iss + R"("})",
                                secret);
  };
  const std::string iss = "https://example.supabase.co/auth/v1";
  EXPECT_TRUE(a.authenticate(token("1800000600", iss)));
  EXPECT_FALSE(a.authenticate(token("1800000600", "https://evil.example/auth/v1")));
  EXPECT_FALSE(a.authenticate(token("1e300", iss)));  // would overflow int64 when cast
  EXPECT_FALSE(a.authenticate(token("-1e300", iss)));
  EXPECT_EQ(a.authenticate("Bearer agent.token.v1")->id, "agent-1");
}

}  // namespace
// TL-SN-002 console lock (ADR 0019 amendment 2026-11-22): a refusal is a
// diagnostic, never an alarm; repeated refusals with the lid closed make the node
// DEGRADED ("USB console access attempts while closed"), never hidden by HEALTHY or
// by a worse status, and they age out after 24 h.
TEST(DeviceRegistry, Sn002ConsoleRefusalsAreEvidenceNotAlarms) {
  DeviceRegistry reg;
  const DeviceId dev("node-sn2");
  const Timestamp t0{10'000'000};
  fw::DeviceEvent beat;
  beat.boot_epoch = 1;
  beat.seq = 1;
  beat.kind = fw::EventKind::kHeartbeat;
  beat.a = 3'000;
  ASSERT_TRUE(reg.observe_unjournaled(dev, beat, t0));
  auto health = [&](Timestamp now) {
    return project_health(dev, reg.find(dev), Freshness::kFresh, now, now, Millis{60'000}, nullptr,
                          false, std::nullopt, true, true);
  };
  auto has = [](const talos::v1::Health& h, const std::string& text) {
    for (const auto& e : h.evidence()) {
      if (e.find(text) != std::string::npos) return true;
    }
    return false;
  };
  EXPECT_EQ(health(t0).status(), talos::v1::HEALTH_STATUS_HEALTHY);

  fw::DeviceEvent refused;
  refused.boot_epoch = 1;
  refused.seq = 2;
  refused.kind = fw::EventKind::kConsoleLocked;
  refused.priority = fw::Priority::kP1State;
  refused.a = 1;
  refused.b = 1;
  ASSERT_TRUE(reg.observe_unjournaled(dev, refused, t0));
  EXPECT_FALSE(reg.observe_unjournaled(dev, refused, t0));  // a replay adds nothing
  EXPECT_EQ(reg.diagnostics(dev).back().kind(), "console.locked");
  EXPECT_EQ(reg.diagnostics(dev).back().value(), 1);
  auto h = health(t0);
  EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_HEALTHY);  // one is a diagnostic only
  EXPECT_TRUE(has(h, "refused while closed"));

  refused.seq = 3;
  refused.a = 4;  // four more, in the node's next rate-limited report
  refused.b = 5;
  ASSERT_TRUE(reg.observe_unjournaled(dev, refused, t0 + Millis{60'000}));
  h = health(t0 + Millis{60'000});
  EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_DEGRADED);
  EXPECT_TRUE(has(h, "USB console access attempts while closed: 5"));
  EXPECT_NE(h.recommended_action().find("USB-C"), std::string::npos);

  // A worse finding keeps the evidence.
  beat.seq = 4;
  beat.a = 2'100;
  beat.b = static_cast<std::uint32_t>(fw::PowerMode::kLowPower);
  ASSERT_TRUE(reg.observe_unjournaled(dev, beat, t0 + Millis{61'000}));
  h = health(t0 + Millis{61'000});
  EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_LOW_POWER);
  EXPECT_TRUE(has(h, "USB console access attempts while closed"));
  beat.seq = 5;
  beat.a = 3'000;
  beat.b = static_cast<std::uint32_t>(fw::PowerMode::kNormal);
  ASSERT_TRUE(reg.observe_unjournaled(dev, beat, t0 + Millis{62'000}));

  // Later the lid is opened: FAULTED (tamper) and the attempts are still named.
  h = project_health(dev, reg.find(dev), Freshness::kFresh, t0, t0 + Millis{62'000}, Millis{60'000},
                     nullptr, true, std::nullopt, true, true);
  EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_FAULTED);
  EXPECT_TRUE(has(h, "USB console access attempts while closed"));

  // A day later, with the node still reporting, they have aged out.
  const Timestamp later = t0 + kConsoleRefusalWindow + Millis{120'000};
  beat.seq = 6;
  ASSERT_TRUE(reg.observe_unjournaled(dev, beat, later));
  h = health(later);
  EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_HEALTHY);
  EXPECT_FALSE(has(h, "USB console"));
}

// TL-SN-002 (ADR 0019): USER-button events are diagnostics (never journaled,
// never acted on); AA low battery asks for new cells; a built-in sensor fault
// does not say "reseat".
TEST(DeviceRegistry, Sn002UserButtonAaLowBatteryAndBuiltInFaults) {
  DeviceRegistry reg;
  const DeviceId dev("node-sn2");
  const Timestamp now{1'000'000};
  fw::DeviceEvent press;
  press.boot_epoch = 1;
  press.seq = 1;
  press.kind = fw::EventKind::kUserButton;
  press.a = static_cast<std::int32_t>(fw::UserButtonAction::kCommissionRequest);
  ASSERT_TRUE(reg.observe_unjournaled(dev, press, now));
  EXPECT_FALSE(reg.observe_unjournaled(dev, press, now));  // a replay changes nothing
  const auto* t = reg.find(dev);
  ASSERT_TRUE(t && t->commission_requested);
  EXPECT_EQ(reg.diagnostics(dev).back().kind(), "user.button");
  auto h = project_health(dev, t, Freshness::kFresh, now, now, Millis{60'000}, nullptr, false,
                          std::nullopt, true, true);
  EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_HEALTHY);
  bool noted = false;
  for (const auto& e : h.evidence()) noted |= e.find("commissioning") != std::string::npos;
  EXPECT_TRUE(noted);

  fw::DeviceEvent beat;
  beat.boot_epoch = 1;
  beat.seq = 2;
  beat.kind = fw::EventKind::kHeartbeat;
  beat.a = 2'150;
  beat.b = static_cast<std::uint32_t>(fw::PowerMode::kLowPower);
  reg.observe(dev, beat, now);
  h = project_health(dev, reg.find(dev), Freshness::kFresh, now, now, Millis{60'000}, nullptr,
                     false, std::nullopt, true, true);
  EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_LOW_POWER);
  EXPECT_NE(h.recommended_action().find("AA"), std::string::npos);
  EXPECT_EQ(h.recommended_action().find("Charge"), std::string::npos);

  beat.seq = 3;
  beat.b = static_cast<std::uint32_t>(fw::PowerMode::kNormal);
  reg.observe(dev, beat, now);
  fw::DeviceEvent fault;
  fault.boot_epoch = 1;
  fault.seq = 4;
  fault.slot = 2;
  fault.kind = fw::EventKind::kModuleFault;
  fault.a = static_cast<std::int32_t>(fw::ModuleFault::kWarmupTimeout);
  ASSERT_TRUE(reg.observe_unjournaled(dev, fault, now));
  h = project_health(dev, reg.find(dev), Freshness::kFresh, now, now, Millis{60'000}, nullptr,
                     false, std::nullopt, true, true);
  EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_FAULTED);
  EXPECT_EQ(h.recommended_action().find("Reseat"), std::string::npos);
  EXPECT_NE(h.evidence(0).find("motion sensor"), std::string::npos);
  // Red team: on USB with no cells the mode stays normal; health must not say healthy.
  beat.seq = 5;
  beat.a = 0;
  beat.b = static_cast<std::uint32_t>(fw::PowerMode::kNormal);
  DeviceRegistry usb_only;
  usb_only.observe(dev, beat, now);
  h = project_health(dev, usb_only.find(dev), Freshness::kFresh, now, now, Millis{60'000}, nullptr,
                     false, std::nullopt, true, true);
  EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_DEGRADED);
  EXPECT_NE(h.recommended_action().find("AA"), std::string::npos);
  // A modular board's 0 mV heartbeat keeps its old meaning.
  h = project_health(dev, usb_only.find(dev), Freshness::kFresh, now, now, Millis{60'000}, nullptr,
                     false, std::nullopt, false, false);
  EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_HEALTHY);
  EXPECT_TRUE(board_has_primary_cells(BoardId("sensor_node_v2")));
  EXPECT_FALSE(board_has_primary_cells(BoardId("sensor_node_proto_a")));
  EXPECT_FALSE(board_has_built_in_sensors(std::nullopt));
}

// TL-SN-002 (ADR 0019 amendment 2026-11-22): a reset through the outside USB-C port
// with the enclosure shut counts like a refused console command; other resets don't.
TEST(DeviceRegistry, Sn002UsbResetWhileClosedIsProbingEvidence) {
  DeviceRegistry reg;
  const DeviceId dev("node-sn2");
  const Timestamp t0{20'000'000};
  fw::DeviceEvent e;
  e.boot_epoch = 1;
  e.seq = 1;
  e.kind = fw::EventKind::kHeartbeat;
  e.a = 3'000;
  ASSERT_TRUE(reg.observe_unjournaled(dev, e, t0));
  auto health = [&](Timestamp now) {
    return project_health(dev, reg.find(dev), Freshness::kFresh, now, now, Millis{60'000}, nullptr,
                          false, std::nullopt, true, true);
  };
  for (std::uint32_t epoch = 2; epoch <= 4; ++epoch) {
    fw::DeviceEvent r;
    r.boot_epoch = epoch;
    r.seq = 1;
    r.kind = fw::EventKind::kReset;
    r.priority = fw::Priority::kP1State;
    r.a = static_cast<std::int32_t>(fw::ResetCause::kUsb);
    r.b = 1;  // enclosure shut
    ASSERT_TRUE(reg.observe_unjournaled(dev, r, t0));
    EXPECT_EQ(reg.diagnostics(dev).back().kind(), "device.reset");
  }
  EXPECT_EQ(health(t0).status(), talos::v1::HEALTH_STATUS_DEGRADED);
  // A power-on reset, or a USB reset with the lid open (someone servicing it), is not probing.
  DeviceRegistry quiet;
  ASSERT_TRUE(quiet.observe_unjournaled(dev, e, t0));
  for (std::uint32_t epoch = 2; epoch <= 5; ++epoch) {
    fw::DeviceEvent r;
    r.boot_epoch = epoch;
    r.seq = 1;
    r.kind = fw::EventKind::kReset;
    r.a = static_cast<std::int32_t>(epoch % 2 ? fw::ResetCause::kPowerOn : fw::ResetCause::kUsb);
    r.b = epoch % 2 ? 1 : 0;
    ASSERT_TRUE(quiet.observe_unjournaled(dev, r, t0));
  }
  EXPECT_EQ(project_health(dev, quiet.find(dev), Freshness::kFresh, t0, t0, Millis{60'000}, nullptr,
                           false, std::nullopt, true, true)
                .status(),
            talos::v1::HEALTH_STATUS_HEALTHY);
}

// Tamper causes other than the lid are named in health (HD-049).
TEST(DeviceRegistry, Sn002TamperCausesAreNamed) {
  DeviceRegistry reg;
  const DeviceId dev("node-sn2");
  const Timestamp t0{30'000'000};
  fw::DeviceEvent e;
  e.boot_epoch = 1;
  e.seq = 1;
  e.kind = fw::EventKind::kHeartbeat;
  e.a = 3'000;
  ASSERT_TRUE(reg.observe_unjournaled(dev, e, t0));
  const auto h = project_health(dev, reg.find(dev), Freshness::kFresh, t0, t0, Millis{60'000},
                                nullptr, true, std::nullopt, true, true, "battery door open");
  EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_FAULTED);
  EXPECT_EQ(h.evidence(0), "tamper: battery door open");
  // The lid alone keeps the classic wording.
  const auto lid = project_health(dev, reg.find(dev), Freshness::kFresh, t0, t0, Millis{60'000},
                                  nullptr, true, std::nullopt, true, true);
  EXPECT_EQ(lid.evidence(0), "enclosure tamper: lid open or loop cut");
}

TEST(HubBridge, EachTamperCauseHasItsOwnCapability) {
  fw::DeviceEvent e;
  e.boot_epoch = 3;
  e.seq = 9;
  e.kind = fw::EventKind::kTamper;
  e.flag = true;
  const std::pair<fw::TamperCause, const char*> cases[] = {
      {fw::TamperCause::kEnclosure, "tamper.enclosure"},
      {fw::TamperCause::kBatteryDoor, "tamper.battery_door"},
      {fw::TamperCause::kMountMoved, "tamper.mount"},
      {fw::TamperCause::kMagnetDefeat, "tamper.magnet"},
      {static_cast<fw::TamperCause>(42), "tamper.enclosure"},  // unknown: fails toward the lid
  };
  for (const auto& [cause, cap] : cases) {
    e.a = static_cast<std::int32_t>(cause);
    e.slot = cause == fw::TamperCause::kMagnetDefeat ? 0 : fw::kNoSlot;
    const auto env = fw::to_envelope(e, DeviceId("node-12"), PropertyId("house-01"));
    ASSERT_TRUE(env.has_value());
    EXPECT_EQ(env->source.capability.str(), cap);
    EXPECT_FALSE(env->source.slot.has_value());  // device-level, whichever sensor saw it
    EXPECT_EQ(env->event_type, "tamper.opened");
  }
  e.kind = fw::EventKind::kReset;
  EXPECT_FALSE(fw::to_envelope(e, DeviceId("node-12"), PropertyId("house-01")).has_value());
}

}  // namespace talos::server
