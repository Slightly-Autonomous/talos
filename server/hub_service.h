// Thread-safe facade over HubEngine for the API layer (ADR 0010).
//
// One mutex serializes every engine call, so the engine stays single-threaded
// and deterministic given its inputs. Accepted commands wait in the engine
// until the target's adapter fetches them; that fetch is the dispatch, so a
// command whose TTL elapsed while the adapter was away is never delivered.
// Full-journal replays (state_at, config at, rule preview) copy entries under
// the mutex and reduce outside it. Live-stream readers wait on a condition
// variable for new journal entries.
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/engine/engine.h"
#include "firmware/portable/types.h"
#include "server/config.h"
#include "server/device_registry.h"
#include "server/metrics.h"
#include "server/persistence/config_store.h"
#include "server/persistence/firmware_store.h"
#include "talos/api/v1/api.pb.h"

namespace talos::server {

class HubService {
 public:
  // With a config store, the latest stored revision is the active config
  // (revision 1 is bootstrapped from the YAML). Without one, config is fixed.
  HubService(LoadedConfig config, std::unique_ptr<IJournal> journal, const IClock& clock,
             std::unique_ptr<SqliteConfigStore> config_store = nullptr,
             std::unique_ptr<SqliteFirmwareStore> firmware_store = nullptr);

  talos::api::v1::HealthResponse health() const;
  talos::v1::Property property() const;
  // Current config, or the revision in force at `at`.
  Result<talos::api::v1::ConfigResponse> config(std::optional<Timestamp> at) const;
  Result<talos::api::v1::ConfigResponse> put_config(const PrincipalId& who,
                                                    const talos::api::v1::PutConfigRequest& req);
  Result<talos::api::v1::RulePreviewResponse> preview_rule(
      const talos::api::v1::RulePreviewRequest& req) const;
  talos::api::v1::StateResponse state() const;
  talos::api::v1::StateResponse state_at(Timestamp t) const;
  talos::api::v1::JournalPage journal_page(std::uint64_t after, std::uint32_t limit) const;

  Result<talos::api::v1::SetArmModeResponse> set_arm_mode(const PrincipalId& who, ArmMode mode);
  Result<talos::api::v1::CommandResponse> request_command(
      const PrincipalId& who, const talos::api::v1::CommandRequest& req);
  Result<talos::api::v1::CommandView> command(const CommandId& id) const;

  // Device ingress: the authenticated device may only speak for itself.
  Result<talos::api::v1::IngestResponse> ingest_observation(
      const DeviceId& device, const talos::v1::ObservationEnvelope& env);
  Result<talos::api::v1::IngestResponse> ingest_ack(const DeviceId& device,
                                                    const talos::v1::CommandAck& ack);
  // Hand-off is the dispatch (ADR 0004): dispatches and returns this device's
  // accepted, unexpired commands. Expired commands are never returned.
  talos::api::v1::PendingCommands take_commands(const DeviceId& device);

  void tick();

  // --- Device network plane (ADR 0014) ---------------------------------------------
  // One authenticated CoAP exchange from `device`: ingests events (domain to
  // the engine, diagnostics to the registry) and returns the downlink items
  // to piggyback: ACKs, pending config, OTA offer/chunks, within budget bytes.
  // `fresh` is false for a frame the ingress classified as a replay or
  // retransmission: its events are still ACKed (and deduplicated), but it
  // gets no config or OTA downlinks and moves no campaign.
  std::vector<fw::Downlink> device_exchange(const DeviceId& device,
                                            const std::vector<fw::DeviceEvent>& events,
                                            std::size_t budget_bytes, bool fresh = true);
  // Whether a verified uplink carries at least one event the hub has not
  // seen (by (boot_epoch, seq)). Replays and retransmissions do not, so the
  // ingress charges them to the sender instead of the device's budget.
  bool uplink_is_fresh(const DeviceId& device, const std::vector<fw::DeviceEvent>& events) const;
  // Owner only: clears the hub's source positions for a device (journaled
  // SourceReset) after a poisoned epoch or a reflash with a rotated key.
  Status reset_device_sequence(const PrincipalId& who, const DeviceId& device);
  // Owner = a human principal with a security-critical grant.
  bool is_owner(const PrincipalId& who) const;
  Result<talos::api::v1::DeviceConfigResponse> set_device_config(
      const PrincipalId& who, const DeviceId& device,
      const talos::api::v1::DeviceConfigRequest& req);
  talos::api::v1::HealthListResponse device_health() const;
  talos::api::v1::DiagnosticsResponse diagnostics(const DeviceId& device) const;
  Status import_firmware(const PrincipalId& who, const talos::v1::FirmwareRelease& manifest,
                         const std::string& image);
  talos::api::v1::FirmwareReleaseList firmware_releases() const;
  Result<talos::api::v1::CampaignResponse> start_campaign(
      const PrincipalId& who, const talos::api::v1::CampaignRequest& req);
  Result<talos::api::v1::CampaignResponse> campaign(const DeviceId& device) const;
  Metrics& metrics() { return metrics_; }
  std::string metrics_text();

  // Blocks until entries after `after` exist or the timeout passes.
  std::vector<JournalEntry> wait_for_entries(std::uint64_t after, std::chrono::milliseconds timeout,
                                             std::size_t limit);
  void shutdown();

  static constexpr std::uint32_t kMaxPage = 500;
  // Per-principal command request budget (red team #8): sustained rate and
  // burst. Denials are journaled for audit only within a smaller budget;
  // beyond it they are answered and counted, not recorded.
  static constexpr double kCommandRatePerSecond = 5;
  static constexpr double kCommandBurst = 20;
  static constexpr double kDenialAuditPerSecond = 0.2;
  static constexpr double kDenialAuditBurst = 20;
  static constexpr std::size_t kMaxAckDetail = 256;
  // HTTP adapters use Unix-second boot epochs; allow this much clock skew.
  static constexpr std::uint32_t kHttpEpochSkewSeconds = 300;

 private:
  void after_mutation_locked();
  talos::api::v1::StateResponse view_locked(const PropertyState& s, Timestamp now) const;

  LoadedConfig config_;
  std::unique_ptr<IJournal> journal_;
  const IClock& clock_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::unique_ptr<SqliteConfigStore> store_;
  std::optional<ConfigRevision> current_;
  std::unique_ptr<HubEngine> engine_;
  std::unique_ptr<SqliteFirmwareStore> firmware_;
  DeviceRegistry registry_;
  std::map<DeviceId, fw::ConfigUpdate> desired_config_;
  // A campaign is pinned to the exact release (board, revision, digest)
  // that passed the compatibility check at start (red team #5).
  struct Campaign {
    std::uint32_t version = 0;
    std::string board_id;
    std::string board_revision;  // the release's ("" = any revision)
    std::string image_sha256;
    std::string state = "scheduled";
    std::optional<Timestamp> last_offer;
  };
  std::map<DeviceId, Campaign> campaigns_;
  Metrics metrics_;
  Status authorize_owner(const PrincipalId& who) const;
  void note_ingest_locked(const DeviceId& device, const IngestResult& r, Timestamp now);
  struct Bucket {
    double tokens = 0;
    std::int64_t last_ms = 0;
  };
  bool take_token_locked(std::map<PrincipalId, Bucket>& buckets, const PrincipalId& who,
                         double rate, double burst, bool consume, Timestamp now);
  std::map<PrincipalId, Bucket> command_buckets_;
  std::map<PrincipalId, Bucket> denial_buckets_;
  bool shutting_down_ = false;
};

}  // namespace talos::server
