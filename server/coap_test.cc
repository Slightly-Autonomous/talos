// Phase 6: real portable firmware on the virtual board <-> real UDP <-> the
// hub's CoAP ingress <-> HubService/HubEngine.
#include "common/coap/coap.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "codec/yaml_loader.h"
#include "firmware/portable/coap_link.h"
#include "firmware/portable/serial_link.h"
#include "firmware/portable/wire.h"
#include "firmware/sim/udp_datagram.h"
#include "firmware/sim/virtual_board.h"
#include "server/coap_server.h"
#include "server/config.h"
#include "server/hub_service.h"
#include "server/persistence/config_store.h"
#include "server/persistence/firmware_store.h"
#include "server/persistence/sqlite_journal.h"
#include "server/release_signing.h"

namespace talos::server {
namespace {

namespace sv = ::talos::server::v1;
namespace api = ::talos::api::v1;

class AtomicClock final : public IClock {
 public:
  explicit AtomicClock(std::int64_t t) : t_(t) {}
  Timestamp now() const override { return {t_.load()}; }
  void advance(std::int64_t ms) { t_ += ms; }

 private:
  std::atomic<std::int64_t> t_;
};

std::string temp_db() {
  static std::atomic<int> n{0};
  const char* dir = std::getenv("TEST_TMPDIR");
  auto p = std::filesystem::path(dir ? dir : "/tmp") / ("coap_" + std::to_string(n++) + ".db");
  std::filesystem::remove(p);
  return p.string();
}

const std::vector<std::uint8_t> kNodeKey = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

struct Fixture {
  explicit Fixture(sv::HubConfig extra = {}) {
    sv::HubConfig h;
    EXPECT_TRUE(codec::parse_yaml(R"(
role: HUB_ROLE_HUB
property_file: contracts/manifests/properties/sample_backyard.yaml
freshness_max_age_ms: 120000
principals:
  - {id: owner, kind: PRINCIPAL_KIND_HUMAN, max_risk: RISK_CLASS_SECURITY_CRITICAL}
  - {id: agent-1, kind: PRINCIPAL_KIND_AGENT, max_risk: RISK_CLASS_SECURITY_CRITICAL}
)",
                                  h));
    h.MergeFrom(extra);
    const std::string db = temp_db();
    auto cfg = build_config(h, ".");
    EXPECT_TRUE(cfg.ok()) << (cfg.ok() ? "" : cfg.error().message);
    hub = std::make_unique<HubService>(cfg.value(), SqliteJournal::open(db).value(), clock,
                                       SqliteConfigStore::open(db).value(),
                                       SqliteFirmwareStore::open(db).value());
    coap = std::make_unique<CoapServer>(*hub, DeviceKeyMap{{"node-12", kNodeKey}}, 1000, 1000);
    port = coap->bind("127.0.0.1", 0).value();
    coap->start();
  }
  ~Fixture() { coap->stop(); }

  AtomicClock clock{1'800'000'000'000};
  std::unique_ptr<HubService> hub;
  std::unique_ptr<CoapServer> coap;
  int port = 0;
};

struct CoapNode {
  explicit CoapNode(int port)
      : udp("127.0.0.1", static_cast<std::uint16_t>(port)),
        link(udp, "node-12", fw::wire::Key{kNodeKey.data(), kNodeKey.size()}),
        node(fw::sim::default_board(), 64 * 1024, &link) {
    node.poll_link = true;
    node.insert(0, fw::ModuleCode::kReedContact);
  }
  fw::sim::UdpDatagram udp;
  fw::CoapLink link;
  fw::sim::Node node;
};

// Interleave virtual device time, hub time and real UDP delivery.
void pump(Fixture& f, CoapNode& n, int ticks) {
  for (int i = 0; i < ticks; ++i) {
    n.node.run_for(10);
    f.clock.advance(10);
    std::this_thread::sleep_for(std::chrono::microseconds(300));
  }
}

// Real UDP between threads: pump until `done` holds, up to `max_ticks`, so a slow runner
// does not lose a reply that lands just after a fixed tick budget.
template <typename Pred>
bool pump_until(Fixture& f, CoapNode& n, int max_ticks, Pred done) {
  for (int i = 0; i < max_ticks; ++i) {
    if (done()) return true;
    pump(f, n, 1);
  }
  return done();
}

std::size_t journaled(HubService& hub, const std::string& type) {
  std::size_t n = 0;
  std::uint64_t after = 0;
  for (;;) {
    auto page = hub.journal_page(after, 500);
    for (const auto& e : page.entries()) {
      n += e.has_observation() && e.observation().envelope().event_type() == type;
    }
    if (!page.has_more()) return n;
    after = page.next_after();
  }
}

TEST(CoapIngress, DebouncedDoorReachesTheJournalOnceAndHealthIsProjected) {
  Fixture f;
  CoapNode n(f.port);
  n.node.boot();
  pump(f, n, 50);
  for (int i = 0; i < 6; ++i) {
    n.node.gpio.set(0, i % 2 == 0);
    pump(f, n, 1);
  }
  n.node.gpio.set(0, true);
  // Wait on the outcome, not a fixed tick budget: a slow runner can delay a UDP reply
  // past a retry. Then keep pumping so a duplicate would have time to show up.
  EXPECT_TRUE(pump_until(f, n, 2000, [&] { return journaled(*f.hub, "contact.opened") >= 1; }));
  pump(f, n, 100);
  EXPECT_EQ(journaled(*f.hub, "contact.opened"), 1u);  // bounce and retries dedupe to one
  EXPECT_EQ(journaled(*f.hub, "contact.state"), 1u);
  EXPECT_EQ(n.link.rejected_replies(), 0u);

  auto health = f.hub->device_health();
  for (const auto& h : health.devices()) {
    if (h.device_id() == "node-12") {
      EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_HEALTHY);
    }
    if (h.device_id() == "patio_speaker") {
      EXPECT_EQ(h.status(), talos::v1::HEALTH_STATUS_OFFLINE);
    }
  }
  EXPECT_FALSE(f.hub->diagnostics(DeviceId("node-12")).diagnostics().empty());
  EXPECT_GT(f.hub->metrics().coap_frames_ok.load(), 0u);
}

TEST(CoapIngress, ConfigPushIsAppliedAndAcknowledged) {
  Fixture f;
  CoapNode n(f.port);
  n.node.boot();
  pump(f, n, 20);
  api::DeviceConfigRequest req;
  req.set_heartbeat_interval_ms(30'000);
  auto r = f.hub->set_device_config(PrincipalId("owner"), DeviceId("node-12"), req);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(f.hub->set_device_config(PrincipalId("agent-1"), DeviceId("node-12"), req).error().code,
            "denied");
  // The node only hears config when it talks: wait for its next heartbeat.
  const auto want = r.value().desired_revision();
  // set_device_config bumps the revision, so only the node's side is polled here.
  EXPECT_TRUE(pump_until(f, n, 12'500, [&] { return n.node.runtime().config().revision == want; }));
  pump(f, n, 50);  // let the applying exchange's ACK reach the hub
  EXPECT_EQ(n.node.runtime().config().revision, want);
  EXPECT_EQ(n.node.runtime().config().heartbeat_interval_ms, 30'000u);
  auto again = f.hub->set_device_config(PrincipalId("owner"), DeviceId("node-12"), req);
  EXPECT_EQ(again.value().applied_revision(), r.value().desired_revision());
}

TEST(CoapIngress, SignedOtaCampaignUpdatesTheNodeEndToEnd) {
  auto kp = generate_keypair();
  ASSERT_TRUE(kp.ok());
  sv::HubConfig extra;
  auto* key = extra.add_firmware_signing_keys();
  key->set_key_id("owner-2026");
  key->set_public_key_hex(kp.value().public_key_hex);
  Fixture f(extra);

  std::string image(6000, '\0');
  for (std::size_t i = 0; i < image.size(); ++i) image[i] = static_cast<char>(i * 7 + 3);
  talos::v1::FirmwareRelease m;
  m.set_board_id("universal_controller_v1");
  m.set_board_revision("proto-a");
  m.set_version("2");
  ASSERT_TRUE(sign_release(m, image, kp.value().seed_hex, "owner-2026"));

  // Rejections first: tampered image, unknown key, agent principal.
  std::string tampered = image;
  tampered[10] ^= 1;
  EXPECT_EQ(f.hub->import_firmware(PrincipalId("owner"), m, tampered).error().code,
            "invalid_release");
  talos::v1::FirmwareRelease other_key = m;
  other_key.set_signing_key_id("someone");
  EXPECT_EQ(f.hub->import_firmware(PrincipalId("owner"), other_key, image).error().code,
            "invalid_release");
  talos::v1::FirmwareRelease bad_sig = m;
  std::string sig = bad_sig.signature();
  sig[0] ^= 1;
  bad_sig.set_signature(sig);
  EXPECT_EQ(f.hub->import_firmware(PrincipalId("owner"), bad_sig, image).error().code,
            "invalid_release");
  EXPECT_EQ(f.hub->import_firmware(PrincipalId("agent-1"), m, image).error().code, "denied");
  ASSERT_TRUE(f.hub->import_firmware(PrincipalId("owner"), m, image));

  CoapNode n(f.port);
  n.node.boot();
  pump(f, n, 20);
  api::CampaignRequest c;
  c.set_device_id("node-12");
  c.set_version(2);
  ASSERT_TRUE(f.hub->start_campaign(PrincipalId("owner"), c).ok());
  // Offer arrives on the next contact (heartbeat), then chunks stream over real UDP. Wait on
  // the outcome: under ASan the hub thread answers slower than virtual time runs, and the
  // node's retries stretch the download past any fixed tick budget (7'000 ticks failed in CI).
  EXPECT_TRUE(pump_until(f, n, 30'000, [&] {
    return n.node.reboots() == 1u && n.node.runtime().running_version() == 2u &&
           n.node.runtime().ota().active_version == 2u;  // confirmed after the post-boot check
  }));
  EXPECT_EQ(n.node.reboots(), 1u);
  EXPECT_EQ(n.node.runtime().running_version(), 2u);
  EXPECT_EQ(n.node.runtime().ota().active_version, 2u);
  // The confirming ACK has to reach the hub over real UDP: wait on it, not a fixed budget.
  pump_until(f, n, 2000,
             [&] { return f.hub->campaign(DeviceId("node-12")).value().state() == "done"; });
  EXPECT_EQ(f.hub->campaign(DeviceId("node-12")).value().state(), "done");
  EXPECT_GT(f.hub->metrics().ota_chunks_sent.load(), 0u);
}

// --- Protocol edge cases (handle() directly) -----------------------------------------------

std::vector<std::uint8_t> request(const std::string& device, const std::vector<std::uint8_t>& key,
                                  std::uint16_t mid, std::uint32_t seq) {
  fw::DeviceEvent e;
  e.boot_epoch = 1;
  e.seq = seq;
  e.slot = 0;
  e.kind = fw::EventKind::kContact;
  e.priority = fw::Priority::kP0Critical;
  e.flag = true;
  std::uint8_t frame[64];
  const auto n =
      fw::wire::encode_uplink(&e, 1, fw::wire::Key{key.data(), key.size()}, frame, sizeof frame);
  coap::Message m;
  m.type = coap::Type::kCon;
  m.code = coap::kPost;
  m.message_id = mid;
  m.token_len = 1;
  m.token[0] = 7;
  m.add_segment("d");
  m.add_segment(device);
  m.add_segment("e");
  m.payload = frame;
  m.payload_len = n;
  std::vector<std::uint8_t> out(256);
  out.resize(coap::build(m, out.data(), out.size()));
  return out;
}

std::uint8_t reply_code(const std::vector<std::uint8_t>& r) {
  coap::Message m;
  return coap::parse(r.data(), r.size(), m) ? m.code : 0;
}

TEST(CoapIngress, ForgedUnknownBadPathDuplicateAndRateLimited) {
  Fixture f;
  CoapServer limited(*f.hub, DeviceKeyMap{{"node-12", kNodeKey}}, 1, 2);
  const std::vector<std::uint8_t> wrong(16, 0xAA);
  EXPECT_EQ(reply_code(f.coap->handle(request("node-12", wrong, 1, 1), "p")), coap::kUnauthorized);
  EXPECT_EQ(reply_code(f.coap->handle(request("ghost", kNodeKey, 2, 1), "p")), coap::kUnauthorized);
  auto bad_path = request("node-12", kNodeKey, 3, 1);
  bad_path[bad_path.size() - 40] ^= 0;  // unchanged, then a separate wrong-path message:
  coap::Message m;
  m.type = coap::Type::kCon;
  m.code = coap::kPost;
  m.message_id = 4;
  m.add_segment("nope");
  std::vector<std::uint8_t> nope(64);
  nope.resize(coap::build(m, nope.data(), nope.size()));
  EXPECT_EQ(reply_code(f.coap->handle(nope, "p")), coap::kNotFound);
  EXPECT_EQ(journaled(*f.hub, "contact.opened"), 0u);  // nothing forged got in

  const auto good = request("node-12", kNodeKey, 9, 1);
  const auto r1 = f.coap->handle(good, "peer-a");
  EXPECT_EQ(reply_code(r1), coap::kChanged);
  const auto frames = f.hub->metrics().coap_frames_ok.load();
  const auto r2 = f.coap->handle(good, "peer-a");  // retransmitted CON
  EXPECT_EQ(r2, r1);
  EXPECT_EQ(f.hub->metrics().coap_frames_ok.load(), frames);
  EXPECT_EQ(journaled(*f.hub, "contact.opened"), 1u);

  EXPECT_EQ(reply_code(limited.handle(request("node-12", kNodeKey, 20, 2), "x")), coap::kChanged);
  EXPECT_EQ(reply_code(limited.handle(request("node-12", kNodeKey, 21, 3), "x")), coap::kChanged);
  EXPECT_EQ(reply_code(limited.handle(request("node-12", kNodeKey, 22, 4), "x")),
            coap::kTooManyRequests);
}

// The bench path before Thread: the node's CoAP datagrams travel as "@F <base64>" console
// lines; a host relay (tools/hil serial_bridge.py on a real board) moves them to the hub's
// UDP port and writes the replies back as lines.
struct SerialRelayNode {
  struct ToHost final : fw::ILineSink {
    std::vector<std::string> lines;
    void write(const char* s, std::size_t n) override { lines.emplace_back(s, n); }
  };
  struct ToNode final : fw::ILineSink {
    fw::SerialDatagram* node_side = nullptr;
    void write(const char* s, std::size_t n) override { node_side->push_line(s, n); }
  };

  explicit SerialRelayNode(int port)
      : udp("127.0.0.1", static_cast<std::uint16_t>(port)),
        node_net(to_host),
        host_net(to_node),
        link(node_net, provisioned_store()),
        node(fw::sim::default_board(), 64 * 1024, &link) {
    to_node.node_side = &node_net;
    node.poll_link = true;
    node.insert(0, fw::ModuleCode::kReedContact);
  }

  fw::sim::Store& provisioned_store() {
    fw::DeviceIdentity id;
    std::strcpy(id.device_id, "node-12");
    id.key_len = static_cast<std::uint8_t>(kNodeKey.size());
    std::copy(kNodeKey.begin(), kNodeKey.end(), id.key);
    fw::save_identity(identity_store, id);
    return identity_store;
  }

  // What the host bridge does each tick: frames to UDP, UDP replies to frames.
  void relay() {
    std::uint8_t buf[fw::SerialDatagram::kMaxDatagram];
    for (const auto& line : to_host.lines) {
      if (host_net.push_line(line.data(), line.size())) {
        while (const auto n = host_net.receive(buf, sizeof buf)) udp.send(buf, n);
      }
    }
    to_host.lines.clear();
    while (const auto n = udp.receive(buf, sizeof buf)) host_net.send(buf, n);
  }

  fw::sim::Store identity_store;
  ToHost to_host;
  ToNode to_node;
  fw::sim::UdpDatagram udp;
  fw::SerialDatagram node_net;
  fw::SerialDatagram host_net;
  fw::ProvisionedLink link;
  fw::sim::Node node;
};

TEST(CoapIngress, SerialConsoleFramesReachTheHubAndAcksComeBack) {
  Fixture f;
  SerialRelayNode n(f.port);
  ASSERT_TRUE(n.link.provisioned());
  n.node.boot();
  auto tick = [&](int ticks) {
    for (int i = 0; i < ticks; ++i) {
      n.node.run_for(10);
      n.relay();
      f.clock.advance(10);
      std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
  };
  tick(50);
  n.node.gpio.set(0, true);  // door opens
  bool seen = false;
  for (int i = 0; i < 400 && !seen; ++i) {
    tick(1);
    seen = journaled(*f.hub, "contact.opened") == 1;
  }
  EXPECT_TRUE(seen);
  for (int i = 0; i < 400 && n.node.runtime().queued() > 0; ++i) tick(1);
  EXPECT_EQ(n.node.runtime().queued(), 0u);  // every event ACKed through the console path
}

}  // namespace
}  // namespace talos::server
