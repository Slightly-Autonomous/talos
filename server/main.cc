// talos_hub: the local property hub process.
//   talos_hub --config hub.yaml
//   talos_hub hash-token <token>     # prints the SHA-256 to put in a config
#include <google/protobuf/util/json_util.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "server/auth.h"
#include "server/coap_server.h"
#include "server/config.h"
#include "server/http_server.h"
#include "server/hub_service.h"
#include "server/persistence/config_store.h"
#include "server/persistence/firmware_store.h"
#include "server/persistence/sqlite_journal.h"
#include "server/release_signing.h"
#include "server/system_clock.h"

namespace {
talos::server::HttpServer* g_server = nullptr;
void on_signal(int) {
  if (g_server) g_server->stop();
}
}  // namespace

int main(int argc, char** argv) {
  using namespace talos::server;
  if (argc == 3 && std::strcmp(argv[1], "hash-token") == 0) {
    std::printf("%s\n", sha256_hex(argv[2]).c_str());
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "keygen") == 0) {
    auto kp = generate_keypair();
    if (!kp) {
      std::fprintf(stderr, "%s\n", kp.error().message.c_str());
      return 2;
    }
    std::printf("seed_hex (SECRET, keep offline): %s\npublic_key_hex: %s\n",
                kp.value().seed_hex.c_str(), kp.value().public_key_hex.c_str());
    return 0;
  }
  if (argc == 9 && std::strcmp(argv[1], "sign-release") == 0) {
    // talos_hub sign-release <seed_hex_file> <key_id> <board_id> <board_rev> <version> <image>
    // <out.json>
    auto slurp = [](const char* path) {
      std::ifstream in(path, std::ios::binary);
      std::stringstream ss;
      ss << in.rdbuf();
      return ss.str();
    };
    std::string seed = slurp(argv[2]);
    while (!seed.empty() && (seed.back() == '\n' || seed.back() == ' ')) seed.pop_back();
    talos::api::v1::FirmwareImportRequest req;
    auto* m = req.mutable_manifest();
    m->set_board_id(argv[4]);
    m->set_board_revision(argv[5]);
    m->set_version(argv[6]);
    req.set_image(slurp(argv[7]));
    if (talos::Status st = sign_release(*m, req.image(), seed, argv[3]); !st) {
      std::fprintf(stderr, "%s\n", st.error().message.c_str());
      return 2;
    }
    std::string json;
    (void)google::protobuf::util::MessageToJsonString(req, &json);
    std::ofstream(argv[8]) << json;
    std::printf("wrote %s (POST it to /v1/firmware/releases)\n", argv[8]);
    return 0;
  }
  if (argc != 3 || std::strcmp(argv[1], "--config") != 0) {
    std::fprintf(stderr,
                 "usage: talos_hub --config <hub.yaml>\n"
                 "       talos_hub hash-token <token>\n"
                 "       talos_hub keygen\n"
                 "       talos_hub sign-release <seed_file> <key_id> <board> <rev> <version> "
                 "<image> <out.json>\n");
    return 2;
  }
  auto cfg = load_config(argv[2]);
  if (!cfg) {
    std::fprintf(stderr, "config error [%s]: %s\n", cfg.error().code.c_str(),
                 cfg.error().message.c_str());
    return 2;
  }
  // Environment overrides for containers/PaaS (Railway sets PORT).
  if (const char* env = std::getenv("TALOS_SQLITE_PATH")) cfg.value().sqlite_path = env;
  auto journal = SqliteJournal::open(cfg.value().sqlite_path);
  if (!journal) {
    std::fprintf(stderr, "storage error [%s]: %s\n", journal.error().code.c_str(),
                 journal.error().message.c_str());
    return 2;
  }
  const auto hub_cfg = cfg.value().hub;
  int port = static_cast<int>(hub_cfg.port() > 0 ? hub_cfg.port() : 8080);
  if (const char* env = std::getenv("PORT")) port = std::atoi(env);
  std::string host = hub_cfg.listen_host().empty() ? "127.0.0.1" : hub_cfg.listen_host();
  if (const char* env = std::getenv("TALOS_LISTEN_HOST")) host = env;

  const bool loopback = host == "127.0.0.1" || host == "localhost" || host == "::1";
  const char* allow_dev = std::getenv("TALOS_ALLOW_DEV_ON_NETWORK");
  if (hub_cfg.dev_mode() && !loopback && !(allow_dev && std::string(allow_dev) == "1")) {
    std::fprintf(stderr,
                 "refusing to listen on %s: config has dev_mode=true (well-known dev tokens).\n"
                 "Use a config with real token hashes, or set TALOS_ALLOW_DEV_ON_NETWORK=1 for a\n"
                 "local container whose port is bound to localhost.\n",
                 host.c_str());
    return 2;
  }

  SystemClock clock;
  const std::string sqlite_path = cfg.value().sqlite_path;
  auto store = SqliteConfigStore::open(sqlite_path);
  if (!store) {
    std::fprintf(stderr, "config store error [%s]: %s\n", store.error().code.c_str(),
                 store.error().message.c_str());
    return 2;
  }
  auto firmware = SqliteFirmwareStore::open(sqlite_path);
  if (!firmware) {
    std::fprintf(stderr, "firmware store error: %s\n", firmware.error().message.c_str());
    return 2;
  }
  const std::string base_dir = cfg.value().base_dir;
  const talos::Property property = cfg.value().engine.property;
  HubService hub(std::move(cfg).value(), std::move(journal).value(), clock,
                 std::move(store).value(), std::move(firmware).value());

  std::unique_ptr<CoapServer> coap;
  if (hub_cfg.coap().enabled()) {
    std::string keys_path = hub_cfg.coap().device_keys_file();
    if (!keys_path.empty() && keys_path[0] != '/') keys_path = base_dir + "/" + keys_path;
    auto keys = load_device_keys(keys_path, property);
    if (!keys) {
      std::fprintf(stderr, "device keys error: %s\n", keys.error().message.c_str());
      return 2;
    }
    coap = std::make_unique<CoapServer>(hub, std::move(keys).value(),
                                        hub_cfg.coap().rate_per_second(), hub_cfg.coap().burst());
    const std::string coap_host =
        hub_cfg.coap().listen_host().empty() ? host : hub_cfg.coap().listen_host();
    auto coap_port = coap->bind(
        coap_host, static_cast<int>(hub_cfg.coap().port() ? hub_cfg.coap().port() : 5683));
    if (!coap_port) {
      std::fprintf(stderr, "could not bind CoAP: %s\n", coap_port.error().message.c_str());
      return 2;
    }
    coap->start();
    std::printf("CoAP device ingress on udp %s:%d\n", coap_host.c_str(), coap_port.value());
  }
  std::string jwt_secret;
  if (hub_cfg.auth() == talos::server::v1::AUTH_MODE_SUPABASE) {
    const char* v = std::getenv(hub_cfg.supabase_auth().jwt_secret_env().c_str());
    if (!v || !*v) {
      std::fprintf(stderr, "auth SUPABASE: env var %s is not set\n",
                   hub_cfg.supabase_auth().jwt_secret_env().c_str());
      return 2;
    }
    jwt_secret = v;
  }
  Authenticator auth(hub_cfg, jwt_secret, &clock);
  HttpServer http(hub, auth, {hub_cfg.cors_origins().begin(), hub_cfg.cors_origins().end()});
  auto bound = http.bind(host, port);
  if (!bound) {
    std::fprintf(stderr, "could not bind %s:%d\n", host.c_str(), port);
    return 2;
  }
  g_server = &http;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::atomic<bool> running{true};
  const auto tick_ms = hub_cfg.tick_interval_ms() > 0 ? hub_cfg.tick_interval_ms() : 250;
  std::thread ticker([&] {
    while (running) {
      hub.tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(tick_ms));
    }
  });

  std::printf("talos_hub listening on %s:%d (journal %s)\n", host.c_str(), bound.value(),
              sqlite_path.c_str());
  std::fflush(stdout);
  http.listen();
  running = false;
  if (coap) coap->stop();
  ticker.join();
  return 0;
}
