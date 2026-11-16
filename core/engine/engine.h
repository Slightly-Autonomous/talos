// HubEngine: the production hub loop, pure and deterministic.
//
//   observation -> ingest (validate, own, dedupe, journal) -> reduce
//     -> verify pending commands -> rules -> policy -> ledger -> outbound
//
// Every decision is journaled before it takes effect, so the journal alone
// rebuilds the engine (constructor restores from it). The engine never reads
// the wall clock; time comes from the injected IClock. Transports, storage and
// the network live outside: the twin drives it today, the Phase 3 server will
// wrap the same class.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/base/result.h"
#include "core/domain/property.h"
#include "core/execution/ledger.h"
#include "core/ingest/ingestor.h"
#include "core/journal/journal.h"
#include "core/policy/policy.h"
#include "core/rules/rules.h"
#include "core/state/state.h"
#include "core/time/time.h"

namespace talos {

struct EngineConfig {
  Property property;
  std::vector<Rule> rules;
  std::vector<Principal> principals;
  // A capability whose last report is older than this is stale.
  Millis freshness_max_age{15 * 60 * 1000};
  // Property-local minute of day for a hub instant. Time-zone/DST policy is
  // supplied by the caller; core does no tz math.
  std::function<std::uint16_t(Timestamp)> local_minute_of_day;
};

class HubEngine {
 public:
  // Restores state and dedup from whatever `journal` already holds.
  HubEngine(EngineConfig config, IJournal& journal, const IClock& clock);
  HubEngine(const HubEngine&) = delete;
  HubEngine& operator=(const HubEngine&) = delete;

  IngestResult on_observation(const ObservationEnvelope& env);
  Result<std::vector<CommandTransition>> on_ack(const CommandAck& ack);
  // Closes TTLs and verification windows that have elapsed.
  void tick();

  Status set_arm_mode(ArmMode mode, const PrincipalId& principal);

  // Journals a ConfigRevisionApplied and swaps property, rules and principals.
  // State is untouched: it derives from the journal, not from configuration.
  // Authorization is the caller's job (authorize_config_change).
  Status apply_config(std::uint32_t revision, const std::string& sha256,
                      const PrincipalId& principal, Property property, std::vector<Rule> rules,
                      std::vector<Principal> principals);
  const EngineConfig& config() const { return config_; }
  // A user/agent command request; principal is taken from the command.
  RequestResult request(const Command& command);

  // Commands dispatched since the last call, for the transport to send.
  std::vector<Command> take_outbound();

  const PropertyState& state() const { return state_; }
  Freshness freshness_of(const CapabilityRef& ref) const;

 private:
  void journal(DomainEvent event);
  void journal_all(const std::vector<CommandTransition>& ts);
  RequestResult submit(const Command& command, const Principal* principal);
  const Principal* find_principal(const PrincipalId& id) const;

  EngineConfig config_;
  IJournal& journal_;
  const IClock& clock_;
  Ingestor ingestor_;
  PropertyState state_;
  std::vector<Command> outbound_;
};

}  // namespace talos
