// HTTP/JSON API v1 and Server-Sent Events over HubService (ADR 0010).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "server/auth.h"
#include "server/hub_service.h"

namespace httplib {
class Server;
}

namespace talos::server {

class HttpServer {
 public:
  HttpServer(HubService& hub, const Authenticator& auth, std::vector<std::string> cors_origins);
  ~HttpServer();

  // Binds; port 0 picks a free port. Returns the bound port or an error.
  Result<int> bind(const std::string& host, int port);
  // Serves until stop(). Blocking.
  bool listen();
  void wait_until_ready();
  void stop();

 private:
  void routes();

  HubService& hub_;
  const Authenticator& auth_;
  std::vector<std::string> cors_origins_;
  std::unique_ptr<httplib::Server> server_;
};

}  // namespace talos::server
