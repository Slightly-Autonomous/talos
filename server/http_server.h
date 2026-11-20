// HTTP/JSON API v1 and Server-Sent Events over HubService (ADR 0010).
#pragma once

#include <atomic>
#include <memory>
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

class HttpServer {
 public:
  HttpServer(HubService& hub, const Authenticator& auth, std::vector<std::string> cors_origins,
             int max_event_streams = kMaxEventStreams);
  ~HttpServer();

  // Binds; port 0 picks a free port. Returns the bound port or an error.
  Result<int> bind(const std::string& host, int port);
  // Serves until stop(). Blocking.
  bool listen();
  void wait_until_ready();
  void stop();

  // Default cap on concurrent /v1/events streams; more get 503 (each holds a
  // worker thread, and the pool has at least 8 base and 32 maximum workers).
  static constexpr int kMaxEventStreams = 16;

 private:
  void routes();

  HubService& hub_;
  const Authenticator& auth_;
  std::vector<std::string> cors_origins_;
  int max_event_streams_;
  std::unique_ptr<httplib::Server> server_;
  // Shared with stream releasers, which may run after a handler returns.
  std::shared_ptr<std::atomic<int>> active_streams_ = std::make_shared<std::atomic<int>>(0);
};

}  // namespace talos::server
