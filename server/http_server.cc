#include "server/http_server.h"

#include <arpa/inet.h>
#include <google/protobuf/util/json_util.h>
#include <httplib.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <set>
#include <thread>

#include "codec/codec.h"

namespace talos::server {

namespace api = ::talos::api::v1;

namespace {

constexpr std::size_t kMaxBodyBytes = 64 * 1024;
constexpr std::size_t kMaxFirmwareBodyBytes = 4 * 1024 * 1024;

std::string to_json(const google::protobuf::Message& m) {
  google::protobuf::util::JsonPrintOptions opts;
  opts.always_print_fields_with_no_presence = true;
  std::string out;
  (void)google::protobuf::util::MessageToJsonString(m, &out, opts);
  return out;
}

void send(httplib::Response& res, int status, const google::protobuf::Message& m) {
  res.status = status;
  res.set_content(to_json(m), "application/json");
}

void send_error(httplib::Response& res, int status, const std::string& code,
                const std::string& message) {
  api::ErrorResponse e;
  e.set_code(code);
  e.set_message(message);
  send(res, status, e);
}

// Slowloris guard (red team #9). cpp-httplib's read timeout restarts on every
// byte, so a client trickling headers holds a worker forever. This server
// gives each connection a deadline for reading a request (headers and body),
// enforced by a watchdog that shuts the socket down, and caps connections per
// peer address. Handlers clear the deadline while they run; streaming
// handlers (SSE) keep it cleared.
class GuardedServer final : public httplib::Server {
 public:
  explicit GuardedServer(HttpServer::ConnectionLimits limits)
      : limits_(limits), watchdog_([this] { watch(); }) {
    new_task_queue = [] { return new httplib::ThreadPool(16, 128, 256); };
  }
  ~GuardedServer() override {
    {
      std::lock_guard lock(mu_);
      stopping_ = true;
    }
    cv_.notify_all();
    watchdog_.join();
  }

  // Called on the worker thread that serves the connection.
  static void headers_received() {
    if (tl_conn_) arm(tl_conn_->read_deadline_s);
  }
  static void handler_entered() { arm(0); }
  // Writing the response, keep-alive idle time and the next request's headers.
  static void handler_left() {
    if (tl_conn_ && !tl_conn_->streaming) arm(3 * tl_conn_->read_deadline_s);
  }
  static void mark_streaming() {
    if (tl_conn_) tl_conn_->streaming = true;
    arm(0);
  }

 protected:
  bool process_and_close_socket(socket_t sock) override {
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    std::string peer = "?";
    if (::getpeername(sock, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
      char host[64] = {};
      if (addr.ss_family == AF_INET) {
        ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&addr)->sin_addr, host, sizeof host);
      } else if (addr.ss_family == AF_INET6) {
        ::inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&addr)->sin6_addr, host,
                    sizeof host);
      }
      peer = host;
    }
    // A dup keeps the open file description alive until we close it, so the
    // watchdog's shutdown() can never hit a reused descriptor.
    Conn conn{::dup(sock), peer, limits_.read_deadline_seconds};
    {
      std::lock_guard lock(mu_);
      if (conn.fd < 0 || per_peer_[peer] >= limits_.max_connections_per_peer) {
        if (conn.fd >= 0) ::close(conn.fd);
        ::shutdown(sock, SHUT_RDWR);
        ::close(sock);
        return false;
      }
      ++per_peer_[peer];
      conns_.insert(&conn);
    }
    tl_conn_ = &conn;
    arm(conn.read_deadline_s);
    const bool ret = httplib::Server::process_and_close_socket(sock);
    tl_conn_ = nullptr;
    {
      std::lock_guard lock(mu_);
      conns_.erase(&conn);
      if (--per_peer_[peer] <= 0) per_peer_.erase(peer);
    }
    ::close(conn.fd);
    return ret;
  }

 private:
  struct Conn {
    int fd;
    std::string peer;
    int read_deadline_s;
    std::atomic<std::int64_t> deadline_ms{0};  // 0: none
    bool streaming = false;
  };
  static std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  static void arm(int seconds) {
    if (tl_conn_) tl_conn_->deadline_ms = seconds > 0 ? now_ms() + seconds * 1000 : 0;
  }
  void watch() {
    std::unique_lock lock(mu_);
    while (!stopping_) {
      cv_.wait_for(lock, std::chrono::milliseconds(200));
      const auto now = now_ms();
      for (Conn* c : conns_) {
        const auto d = c->deadline_ms.load();
        if (d != 0 && now > d) {
          ::shutdown(c->fd, SHUT_RDWR);  // the worker's read fails and it closes
          c->deadline_ms = 0;
        }
      }
    }
  }

  static thread_local Conn* tl_conn_;
  const HttpServer::ConnectionLimits limits_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stopping_ = false;
  std::set<Conn*> conns_;
  std::map<std::string, int> per_peer_;
  std::thread watchdog_;
};

thread_local GuardedServer::Conn* GuardedServer::tl_conn_ = nullptr;

// RAII marker for "a handler is running on this connection".
struct HandlerScope {
  HandlerScope() { GuardedServer::handler_entered(); }
  ~HandlerScope() { GuardedServer::handler_left(); }
  HandlerScope(const HandlerScope&) = delete;
  HandlerScope& operator=(const HandlerScope&) = delete;
};

}  // namespace

int http_status_for(const Error& e) {
  if (e.code == "forbidden" || e.code == "denied") return 403;
  if (e.code == "not_found") return 404;
  if (e.code == "unknown_principal") return 403;
  if (e.code == "conflict" || e.code == "stale_revision") return 409;
  if (e.code == "rate_limited") return 429;
  // Hub-side failures are not the caller's fault: never report them as 400.
  if (e.code == "storage_error" || e.code == "corrupt_journal" || e.code == "encode_error" ||
      e.code == "crypto_unavailable" || e.code == "schema_too_new" ||
      e.code == "invalid_journal_entry" || e.code == "internal") {
    return 500;
  }
  return 400;
}

namespace {

void send_error(httplib::Response& res, const Error& e) {
  send_error(res, http_status_for(e), e.code, e.message);
}

bool parse_body(const httplib::Request& req, httplib::Response& res,
                google::protobuf::Message& out) {
  const bool firmware = req.path.rfind("/v1/firmware/releases", 0) == 0;
  if (req.body.size() > (firmware ? kMaxFirmwareBodyBytes : kMaxBodyBytes)) {
    send_error(res, 413, "payload_too_large", "request body too large");
    return false;
  }
  google::protobuf::util::JsonParseOptions opts;
  opts.ignore_unknown_fields = false;
  auto st = google::protobuf::util::JsonStringToMessage(req.body, &out, opts);
  if (!st.ok()) {
    send_error(res, 400, "invalid_json", std::string(st.message()));
    return false;
  }
  return true;
}

std::uint64_t query_u64(const httplib::Request& req, const char* key, std::uint64_t fallback) {
  if (!req.has_param(key)) return fallback;
  const std::string v = req.get_param_value(key);
  std::uint64_t out = fallback;
  std::from_chars(v.data(), v.data() + v.size(), out);
  return out;
}

}  // namespace

HttpServer::HttpServer(HubService& hub, const Authenticator& auth,
                       std::vector<std::string> cors_origins, int max_event_streams,
                       ConnectionLimits limits)
    : hub_(hub),
      auth_(auth),
      cors_origins_(std::move(cors_origins)),
      max_event_streams_(max_event_streams),
      server_(std::make_unique<GuardedServer>(limits)) {
  server_->set_payload_max_length(kMaxFirmwareBodyBytes);
  routes();
}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::acquire_stream(const PrincipalId& who) {
  const bool owner = hub_.is_owner(who);
  std::lock_guard lock(streams_->mu);
  const int reserved = std::min(kReservedOwnerStreams, max_event_streams_ / 2);
  const int limit = owner ? max_event_streams_ : max_event_streams_ - reserved;
  int& mine = streams_->per_principal[who];
  if (streams_->total >= limit || mine >= kMaxStreamsPerPrincipal) {
    if (mine == 0) streams_->per_principal.erase(who);
    return false;
  }
  ++mine;
  ++streams_->total;
  return true;
}

Result<int> HttpServer::bind(const std::string& host, int port) {
  if (port == 0) {
    const int p = server_->bind_to_any_port(host);
    if (p <= 0) return Error{"bind_failed", host};
    return p;
  }
  if (!server_->bind_to_port(host, port))
    return Error{"bind_failed", host + ":" + std::to_string(port)};
  return port;
}

bool HttpServer::listen() { return server_->listen_after_bind(); }

void HttpServer::wait_until_ready() { server_->wait_until_ready(); }

void HttpServer::stop() {
  hub_.shutdown();
  if (server_) server_->stop();
}

void HttpServer::routes() {
  auto& s = *server_;

  // CORS for configured origins only.
  s.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
    const std::string origin = req.get_header_value("Origin");
    if (!origin.empty() &&
        std::find(cors_origins_.begin(), cors_origins_.end(), origin) != cors_origins_.end()) {
      res.set_header("Access-Control-Allow-Origin", origin);
      res.set_header("Vary", "Origin");
      res.set_header("Access-Control-Allow-Headers", "Authorization, Content-Type");
      res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS");
    }
    ++hub_.metrics().http_requests;
    GuardedServer::headers_received();  // the body must follow within the deadline
    if (req.method == "OPTIONS") {
      res.status = 204;
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  // Wraps a handler that requires a principal (person/agent) identity.
  auto principal = [this](auto handler) {
    return [this, handler](const httplib::Request& req, httplib::Response& res) {
      HandlerScope scope;
      auto id = auth_.authenticate(req.get_header_value("Authorization"));
      if (!id) return send_error(res, 401, "unauthenticated", "missing or invalid bearer token");
      if (id->kind != Identity::Kind::kPrincipal) {
        return send_error(res, 403, "forbidden", "device credentials cannot use the user API");
      }
      handler(PrincipalId(id->id), req, res);
    };
  };
  auto device = [this](auto handler) {
    return [this, handler](const httplib::Request& req, httplib::Response& res) {
      HandlerScope scope;
      auto id = auth_.authenticate(req.get_header_value("Authorization"));
      if (!id) return send_error(res, 401, "unauthenticated", "missing or invalid bearer token");
      if (id->kind != Identity::Kind::kDevice) {
        return send_error(res, 403, "forbidden", "only device credentials may use device ingress");
      }
      handler(DeviceId(id->id), req, res);
    };
  };

  s.Get("/v1/health", [this](const httplib::Request&, httplib::Response& res) {
    HandlerScope scope;
    send(res, 200, hub_.health());
  });

  s.Get("/v1/property",
        principal([this](const PrincipalId&, const httplib::Request&, httplib::Response& res) {
          send(res, 200, hub_.property());
        }));

  s.Get("/v1/state", principal([this](const PrincipalId&, const httplib::Request&,
                                      httplib::Response& res) { send(res, 200, hub_.state()); }));

  s.Get("/v1/state_at",
        principal([this](const PrincipalId&, const httplib::Request& req, httplib::Response& res) {
          if (!req.has_param("t")) return send_error(res, 400, "invalid_request", "t is required");
          const auto t = static_cast<std::int64_t>(query_u64(req, "t", 0));
          send(res, 200, hub_.state_at(Timestamp{t}));
        }));

  s.Get("/v1/journal",
        principal([this](const PrincipalId&, const httplib::Request& req, httplib::Response& res) {
          send(res, 200,
               hub_.journal_page(query_u64(req, "after", 0),
                                 static_cast<std::uint32_t>(query_u64(req, "limit", 100))));
        }));

  // Live journal stream. Clients must send the bearer header (use fetch
  // streaming, not EventSource): tokens never go in URLs.
  // Each stream holds a worker thread, so streams are capped well below the
  // pool size: device ingest and commands must always find a free worker.
  s.Get("/v1/events", principal([this](const PrincipalId& who, const httplib::Request& req,
                                       httplib::Response& res) {
          if (!acquire_stream(who)) {
            res.set_header("Retry-After", "5");
            return send_error(res, 503, "too_many_streams", "too many live event streams");
          }
          GuardedServer::mark_streaming();
          auto cursor = std::make_shared<std::uint64_t>(query_u64(req, "after", 0));
          res.set_header("Cache-Control", "no-cache");
          auto streams = streams_;
          res.set_chunked_content_provider(
              "text/event-stream",
              [this, cursor](std::size_t, httplib::DataSink& sink) {
                auto entries = hub_.wait_for_entries(*cursor, std::chrono::seconds(15), 100);
                if (entries.empty()) {
                  const std::string ping = ": keepalive\n\n";
                  return sink.write(ping.data(), ping.size());
                }
                for (const auto& e : entries) {
                  const std::string msg = "id: " + std::to_string(e.journal_seq) +
                                          "\nevent: journal\ndata: " + to_json(codec::to_proto(e)) +
                                          "\n\n";
                  if (!sink.write(msg.data(), msg.size())) return false;
                  *cursor = e.journal_seq;
                }
                return true;
              },
              [streams, who](bool) {
                std::lock_guard lock(streams->mu);
                --streams->total;
                if (--streams->per_principal[who] <= 0) streams->per_principal.erase(who);
              });
        }));

  s.Get("/v1/config",
        principal([this](const PrincipalId&, const httplib::Request& req, httplib::Response& res) {
          std::optional<Timestamp> at;
          if (req.has_param("at")) {
            at = Timestamp{static_cast<std::int64_t>(query_u64(req, "at", 0))};
          }
          auto r = hub_.config(at);
          if (!r) return send_error(res, r.error());
          send(res, 200, r.value());
        }));

  s.Put("/v1/config", principal([this](const PrincipalId& who, const httplib::Request& req,
                                       httplib::Response& res) {
          api::PutConfigRequest body;
          if (!parse_body(req, res, body)) return;
          auto r = hub_.put_config(who, body);
          if (!r) return send_error(res, r.error());
          send(res, 200, r.value());
        }));

  s.Post("/v1/rules/preview",
         principal([this](const PrincipalId&, const httplib::Request& req, httplib::Response& res) {
           api::RulePreviewRequest body;
           if (!parse_body(req, res, body)) return;
           auto r = hub_.preview_rule(body);
           if (!r) return send_error(res, r.error());
           send(res, 200, r.value());
         }));

  s.Get("/v1/health/devices",
        principal([this](const PrincipalId&, const httplib::Request&, httplib::Response& res) {
          send(res, 200, hub_.device_health());
        }));

  s.Get("/v1/diagnostics",
        principal([this](const PrincipalId&, const httplib::Request& req, httplib::Response& res) {
          if (!req.has_param("device_id")) {
            return send_error(res, 400, "invalid_request", "device_id is required");
          }
          send(res, 200, hub_.diagnostics(DeviceId(req.get_param_value("device_id"))));
        }));

  // Camera media (ADR 0020): a snapshot or clip from the recorder, for a
  // human with media access, and only for a media id this device's journaled
  // observations named. The id's character set is enforced at ingest, so it
  // can never become another recorder path.
  s.Get(
      R"(/v1/media/([A-Za-z0-9_\-]+)/([A-Za-z0-9_\-][A-Za-z0-9._\-]{0,127})/(snapshot\.jpg|clip\.mp4))",
      principal(
          [this](const PrincipalId& who, const httplib::Request& req, httplib::Response& res) {
            const DeviceId device(req.matches[1]);
            const std::string media_id = req.matches[2];
            const std::string kind = req.matches[3];
            if (Status st = hub_.authorize_media(who, device, media_id); !st) {
              return send_error(res, st.error());
            }
            const auto& mc = hub_.media_config();
            if (mc.frigate_url().empty()) {
              return send_error(res, 503, "media_unavailable", "no recorder is configured");
            }
            httplib::Client frigate(mc.frigate_url());
            frigate.set_connection_timeout(3, 0);
            frigate.set_read_timeout(kind == "clip.mp4" ? 30 : 5, 0);
            const std::size_t limit = kind == "clip.mp4"
                                          ? (mc.max_clip_bytes() ? mc.max_clip_bytes() : 64u << 20)
                                          : (8u << 20);
            std::string body;
            bool too_big = false;
            auto r = frigate.Get("/api/events/" + media_id + "/" + kind,
                                 [&](const char* data, std::size_t n) {
                                   if (body.size() + n > limit) {
                                     too_big = true;
                                     return false;
                                   }
                                   body.append(data, n);
                                   return true;
                                 });
            if (too_big) return send_error(res, 502, "media_too_large", "recorder media too large");
            if (!r) return send_error(res, 502, "recorder_unreachable", "recorder did not answer");
            if (r->status == 404)
              return send_error(res, 404, "not_found", "recorder has no " + kind);
            if (r->status != 200) {
              return send_error(res, 502, "recorder_error",
                                "recorder answered " + std::to_string(r->status));
            }
            res.status = 200;
            res.set_header("Cache-Control", "private, max-age=300");
            res.set_content(std::move(body), kind == "clip.mp4" ? "video/mp4" : "image/jpeg");
          }));

  // Owner only: forget a device's source positions (journaled SourceReset),
  // after a poisoned boot epoch or a reflash with a rotated key (ADR 0008).
  s.Post(R"(/v1/devices/([A-Za-z0-9_\-]+)/sequence/reset)",
         principal(
             [this](const PrincipalId& who, const httplib::Request& req, httplib::Response& res) {
               if (Status st = hub_.reset_device_sequence(who, DeviceId(req.matches[1])); !st) {
                 return send_error(res, st.error());
               }
               api::IngestResponse out;
               out.set_status("reset");
               send(res, 200, out);
             }));

  s.Post(R"(/v1/devices/([A-Za-z0-9_\-]+)/config)",
         principal(
             [this](const PrincipalId& who, const httplib::Request& req, httplib::Response& res) {
               api::DeviceConfigRequest body;
               if (!parse_body(req, res, body)) return;
               auto r = hub_.set_device_config(who, DeviceId(req.matches[1]), body);
               if (!r) return send_error(res, r.error());
               send(res, 200, r.value());
             }));

  s.Post("/v1/firmware/releases",
         principal(
             [this](const PrincipalId& who, const httplib::Request& req, httplib::Response& res) {
               api::FirmwareImportRequest body;
               if (!parse_body(req, res, body)) return;
               if (Status st = hub_.import_firmware(who, body.manifest(), body.image()); !st) {
                 return send_error(res, st.error());
               }
               send(res, 201, hub_.firmware_releases());
             }));

  s.Get("/v1/firmware/releases",
        principal([this](const PrincipalId&, const httplib::Request&, httplib::Response& res) {
          send(res, 200, hub_.firmware_releases());
        }));

  s.Post("/v1/firmware/campaigns",
         principal(
             [this](const PrincipalId& who, const httplib::Request& req, httplib::Response& res) {
               api::CampaignRequest body;
               if (!parse_body(req, res, body)) return;
               auto r = hub_.start_campaign(who, body);
               if (!r) return send_error(res, r.error());
               send(res, 202, r.value());
             }));

  s.Get(R"(/v1/firmware/campaigns/([A-Za-z0-9_\-]+))",
        principal([this](const PrincipalId&, const httplib::Request& req, httplib::Response& res) {
          auto r = hub_.campaign(DeviceId(req.matches[1]));
          if (!r) return send_error(res, r.error());
          send(res, 200, r.value());
        }));

  // Prometheus scrape (bearer token: configure bearer_token in Prometheus).
  s.Get("/metrics",
        principal([this](const PrincipalId&, const httplib::Request&, httplib::Response& res) {
          res.set_content(hub_.metrics_text(), "text/plain; version=0.0.4");
        }));

  s.Post("/v1/arm", principal([this](const PrincipalId& who, const httplib::Request& req,
                                     httplib::Response& res) {
           api::SetArmModeRequest body;
           if (!parse_body(req, res, body)) return;
           auto mode = codec::from_proto(body.mode());
           if (!mode) return send_error(res, mode.error());
           auto r = hub_.set_arm_mode(who, mode.value());
           if (!r) return send_error(res, r.error());
           send(res, 200, r.value());
         }));

  s.Post("/v1/commands", principal([this](const PrincipalId& who, const httplib::Request& req,
                                          httplib::Response& res) {
           api::CommandRequest body;
           if (!parse_body(req, res, body)) return;
           auto r = hub_.request_command(who, body);
           if (!r) return send_error(res, r.error());
           // 202: accepted for processing; the outcome is reported by status.
           send(res, 202, r.value());
         }));

  s.Get(R"(/v1/commands/([A-Za-z0-9_:@.\-]+))",
        principal([this](const PrincipalId&, const httplib::Request& req, httplib::Response& res) {
          auto r = hub_.command(CommandId(req.matches[1]));
          if (!r) return send_error(res, r.error());
          send(res, 200, r.value());
        }));

  s.Post("/v1/devices/observations",
         device([this](const DeviceId& dev, const httplib::Request& req, httplib::Response& res) {
           talos::v1::ObservationEnvelope body;
           if (!parse_body(req, res, body)) return;
           auto r = hub_.ingest_observation(dev, body);
           if (!r) return send_error(res, r.error());
           send(res, r.value().status() == "rejected" ? 422 : 200, r.value());
         }));

  s.Post("/v1/devices/acks",
         device([this](const DeviceId& dev, const httplib::Request& req, httplib::Response& res) {
           talos::v1::CommandAck body;
           if (!parse_body(req, res, body)) return;
           auto r = hub_.ingest_ack(dev, body);
           if (!r) return send_error(res, r.error());
           send(res, 200, r.value());
         }));

  s.Get("/v1/devices/commands",
        device([this](const DeviceId& dev, const httplib::Request&, httplib::Response& res) {
          send(res, 200, hub_.take_commands(dev));
        }));
}

}  // namespace talos::server
