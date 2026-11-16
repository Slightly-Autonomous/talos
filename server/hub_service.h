// Thread-safe facade over HubEngine for the API layer (ADR 0010).
//
// One mutex serializes every engine call, so the engine stays single-threaded
// and deterministic given its inputs. Commands the engine dispatches are
// queued per target device until a device adapter fetches them; live-stream
// readers wait on a condition variable for new journal entries.
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/engine/engine.h"
#include "server/config.h"
#include "talos/api/v1/api.pb.h"

namespace talos::server {

class HubService {
 public:
  HubService(LoadedConfig config, std::unique_ptr<IJournal> journal, const IClock& clock);

  talos::api::v1::HealthResponse health() const;
  talos::v1::Property property() const { return config_.property_proto; }
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
  talos::api::v1::PendingCommands take_commands(const DeviceId& device);

  void tick();

  // Blocks until entries after `after` exist or the timeout passes.
  std::vector<JournalEntry> wait_for_entries(std::uint64_t after, std::chrono::milliseconds timeout,
                                             std::size_t limit);
  void shutdown();

  static constexpr std::uint32_t kMaxPage = 500;

 private:
  void after_mutation_locked();
  talos::api::v1::StateResponse view_locked(const PropertyState& s, Timestamp now) const;

  LoadedConfig config_;
  std::unique_ptr<IJournal> journal_;
  const IClock& clock_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  HubEngine engine_;
  std::map<DeviceId, std::deque<Command>> outbound_;
  bool shutting_down_ = false;
};

}  // namespace talos::server
