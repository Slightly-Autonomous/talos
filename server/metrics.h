// Process metrics in Prometheus text format (spec §27). Labels are bounded
// (fixed outcome names only: never device or event ids).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace talos::server {

struct Metrics {
  std::atomic<std::uint64_t> http_requests{0};
  std::atomic<std::uint64_t> ingest_accepted{0};
  std::atomic<std::uint64_t> ingest_duplicate{0};
  std::atomic<std::uint64_t> ingest_rejected{0};
  std::atomic<std::uint64_t> coap_frames_ok{0};
  std::atomic<std::uint64_t> coap_bad_request{0};
  std::atomic<std::uint64_t> coap_auth_failed{0};
  std::atomic<std::uint64_t> coap_rate_limited{0};
  std::atomic<std::uint64_t> coap_duplicate_messages{0};
  std::atomic<std::uint64_t> ota_chunks_sent{0};
  std::atomic<std::uint64_t> journal_append_failures{0};
  std::atomic<std::uint64_t> journal_seq{0};

  std::string prometheus() const;
};

}  // namespace talos::server
