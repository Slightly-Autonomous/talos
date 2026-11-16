// talos_hub: the local property hub process.
//   talos_hub --config hub.yaml
//   talos_hub hash-token <token>     # prints the SHA-256 to put in a config
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "server/auth.h"
#include "server/config.h"
#include "server/http_server.h"
#include "server/hub_service.h"
#include "server/persistence/sqlite_journal.h"
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
  if (argc != 3 || std::strcmp(argv[1], "--config") != 0) {
    std::fprintf(stderr, "usage: talos_hub --config <hub.yaml> | talos_hub hash-token <token>\n");
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
  HubService hub(std::move(cfg).value(), std::move(journal).value(), clock);
  Authenticator auth(hub_cfg);
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
  ticker.join();
  return 0;
}
