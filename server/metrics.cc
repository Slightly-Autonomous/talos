#include "server/metrics.h"

namespace talos::server {

std::string Metrics::prometheus() const {
  std::string out;
  auto counter = [&](const char* name, const char* help, const char* labels, std::uint64_t v) {
    out += std::string("# HELP ") + name + " " + help + "\n# TYPE " + name + " counter\n";
    out += std::string(name) + labels + " " + std::to_string(v) + "\n";
  };
  counter("talos_http_requests_total", "HTTP API requests.", "", http_requests);
  out +=
      "# HELP talos_ingest_total Device observations by outcome.\n# TYPE talos_ingest_total "
      "counter\n";
  out += "talos_ingest_total{outcome=\"accepted\"} " + std::to_string(ingest_accepted) + "\n";
  out += "talos_ingest_total{outcome=\"duplicate\"} " + std::to_string(ingest_duplicate) + "\n";
  out += "talos_ingest_total{outcome=\"rejected\"} " + std::to_string(ingest_rejected) + "\n";
  out +=
      "# HELP talos_coap_frames_total CoAP device frames by outcome.\n# TYPE "
      "talos_coap_frames_total counter\n";
  out += "talos_coap_frames_total{outcome=\"ok\"} " + std::to_string(coap_frames_ok) + "\n";
  out +=
      "talos_coap_frames_total{outcome=\"bad_request\"} " + std::to_string(coap_bad_request) + "\n";
  out +=
      "talos_coap_frames_total{outcome=\"auth_failed\"} " + std::to_string(coap_auth_failed) + "\n";
  out += "talos_coap_frames_total{outcome=\"rate_limited\"} " + std::to_string(coap_rate_limited) +
         "\n";
  out += "talos_coap_frames_total{outcome=\"duplicate_message\"} " +
         std::to_string(coap_duplicate_messages) + "\n";
  counter("talos_ota_chunks_sent_total", "Firmware chunks sent to devices.", "", ota_chunks_sent);
  counter("talos_journal_append_failures_total", "Journal writes that failed (fail closed).", "",
          journal_append_failures);
  out += "# HELP talos_journal_seq Last journal sequence number.\n# TYPE talos_journal_seq gauge\n";
  out += "talos_journal_seq " + std::to_string(journal_seq) + "\n";
  return out;
}

}  // namespace talos::server
