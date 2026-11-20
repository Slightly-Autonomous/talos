// HTTP/JSON API v1 and Server-Sent Events over HubService (ADR 0010).
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "server/auth.h"
#include "server/hub_service.h"

namespace httplib {
class Server;
}

namespace talos::server {

// HTTP status for a hub error: 4xx for caller errors, 500 for hub failures
// (storage, corrupt journal, encoding), never 400 for something the caller
// cannot fix.
int http_status_for(const Error& e);

// Slowloris guard (red team #9): a connection must deliver a request's
// headers and body within read_deadline_seconds, and one peer address may
// hold at most max_connections_per_peer connections. Put the documented TLS
// proxy in front for anything beyond a LAN (it is then the single peer
// address, so raise the cap there).
struct HttpConnectionLimits {
  int read_deadline_seconds = 10;
  int max_connections_per_peer = 32;
};

class HttpServer {
 public:
  using ConnectionLimits = HttpConnectionLimits;

  HttpServer(HubService& hub, const Authenticator& auth, std::vector<std::string> cors_origins,
             int max_event_streams = kMaxEventStreams, ConnectionLimits limits = {});
  ~HttpServer();

  // Binds; port 0 picks a free port. Returns the bound port or an error.
  Result<int> bind(const std::string& host, int port);
  // Serves until stop(). Blocking.
  bool listen();
  void wait_until_ready();
  void stop();

  // Default cap on concurrent /v1/events streams; more get 503 (each holds a
  // worker thread; the pool has 16 base and 128 maximum workers).
  static constexpr int kMaxEventStreams = 16;
  // Each principal may hold at most this many streams, and the last
  // kReservedOwnerStreams slots are kept for owners (red team #11).
  static constexpr int kMaxStreamsPerPrincipal = 4;
  static constexpr int kReservedOwnerStreams = 4;

 private:
  void routes();
  bool acquire_stream(const PrincipalId& who);
  void release_stream(const PrincipalId& who);

  HubService& hub_;
  const Authenticator& auth_;
  std::vector<std::string> cors_origins_;
  int max_event_streams_;
  std::unique_ptr<httplib::Server> server_;
  struct Streams {
    std::mutex mu;
    int total = 0;
    std::map<PrincipalId, int> per_principal;
  };
  // Shared with stream releasers, which may run after a handler returns.
  std::shared_ptr<Streams> streams_ = std::make_shared<Streams>();
};

}  // namespace talos::server
