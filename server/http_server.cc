#include "server/http_server.h"

#include <google/protobuf/util/json_util.h>
#include <httplib.h>

#include <algorithm>
#include <charconv>

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

int status_for(const Error& e) {
  if (e.code == "forbidden" || e.code == "denied") return 403;
  if (e.code == "not_found") return 404;
  if (e.code == "unknown_principal") return 403;
  if (e.code == "conflict") return 409;
  if (e.code == "invalid_release") return 400;
  return 400;
}

void send_error(httplib::Response& res, const Error& e) {
  send_error(res, status_for(e), e.code, e.message);
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
                       std::vector<std::string> cors_origins)
    : hub_(hub),
      auth_(auth),
      cors_origins_(std::move(cors_origins)),
      server_(std::make_unique<httplib::Server>()) {
  server_->set_payload_max_length(kMaxFirmwareBodyBytes);
  routes();
}

HttpServer::~HttpServer() { stop(); }

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
    if (req.method == "OPTIONS") {
      res.status = 204;
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  // Wraps a handler that requires a principal (person/agent) identity.
  auto principal = [this](auto handler) {
    return [this, handler](const httplib::Request& req, httplib::Response& res) {
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
      auto id = auth_.authenticate(req.get_header_value("Authorization"));
      if (!id) return send_error(res, 401, "unauthenticated", "missing or invalid bearer token");
      if (id->kind != Identity::Kind::kDevice) {
        return send_error(res, 403, "forbidden", "only device credentials may use device ingress");
      }
      handler(DeviceId(id->id), req, res);
    };
  };

  s.Get("/v1/health",
        [this](const httplib::Request&, httplib::Response& res) { send(res, 200, hub_.health()); });

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
  s.Get("/v1/events",
        principal([this](const PrincipalId&, const httplib::Request& req, httplib::Response& res) {
          auto cursor = std::make_shared<std::uint64_t>(query_u64(req, "after", 0));
          res.set_header("Cache-Control", "no-cache");
          res.set_chunked_content_provider(
              "text/event-stream", [this, cursor](std::size_t, httplib::DataSink& sink) {
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
