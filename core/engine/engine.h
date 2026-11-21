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

  // A journal failure after the observation was recorded is reported in
  // `error`, and rules do not run for it.
  // Only an observation that becomes the capability's current state (newest
  // source position) verifies commands or triggers rules; a superseded one is
  // journaled as history and flagged in the result.
  IngestResult on_observation(const ObservationEnvelope& env, IngestOptions options = {});
  Result<std::vector<CommandTransition>> on_ack(const CommandAck& ack);
  // Closes TTLs and verification windows that have elapsed, and ends
  // maintenance windows whose time is up (a lid still open then alarms).
  Status tick();
  // True while the device's maintenance window is open at the engine's clock.
  bool in_maintenance(const DeviceId& device) const;

  Status set_arm_mode(ArmMode mode, const PrincipalId& principal);

  // Journals a SourceReset for the device and forgets its source positions,
  // so its next report is heard again. Authorization is the caller's job
  // (owner only: it reopens the dedup window for that device).
  Status reset_source(const DeviceId& device, const PrincipalId& principal);

  // Journals a ConfigRevisionApplied and swaps property, rules and principals.
  // State is untouched: it derives from the journal, not from configuration.
  // Authorization is the caller's job (authorize_config_change). Nothing
  // changes if the journal append fails.
  Status apply_config(std::uint32_t revision, const std::string& sha256,
                      const PrincipalId& principal, Property property, std::vector<Rule> rules,
                      std::vector<Principal> principals);
  const EngineConfig& config() const { return config_; }
  // A user/agent command request; principal is taken from the command.
  // The command is checked against the operation catalog (args, target,
  // TTL, window, expect) and denied on mismatch before policy runs; the
  // recorded command carries the hub-derived expectation. An accepted command
  // waits for hand-off: it is not dispatched here.
  // With record_denial=false a denied request is answered but not journaled
  // (the caller's audit budget for that principal is spent).
  RequestResult request(const Command& command, bool record_denial = true);

  // Hand-off to the transport IS the dispatch (ADR 0004): accepted commands
  // are dispatched now, in request order, and returned for sending. Commands
  // whose TTL has elapsed become `expired` and are never returned.
  std::vector<Command> take_outbound();
  // Same, for commands targeting one device (a polling adapter).
  std::vector<Command> take_outbound(const DeviceId& device);

  const PropertyState& state() const { return state_; }
  // Whether a source position from this device is unseen and in range.
  bool is_new_position(const DeviceId& device, SourcePosition pos) const {
    return ingestor_.is_new(device, pos);
  }
  Freshness freshness_of(const CapabilityRef& ref) const;

 private:
  Status journal(DomainEvent event);
  Status journal_all(const std::vector<CommandTransition>& ts);
  enum class Origin { kRequest, kRule };
  RequestResult submit(const Command& command, Origin origin, bool record_denial = true);
  // Runs rules for a current observation; suppresses tamper alarms of nodes in
  // maintenance (journaled as AlarmSuppressed).
  void run_rules(const ObservationEnvelope& env, IngestResult& r);
  // Executes an accepted hub-internal command (device.maintenance*).
  Status execute_internal(const CommandId& id);
  // Maintenance ended: if the node's lid is still open, the tamper alarm fires now.
  Status alarm_if_tamper_open(const DeviceId& device);
  Authorization authorize(const Command& command, Origin origin, Command& normalized) const;
  std::vector<Command> hand_off(const DeviceId* device);
  const Principal* find_principal(const PrincipalId& id) const;

  EngineConfig config_;
  IJournal& journal_;
  const IClock& clock_;
  Ingestor ingestor_;
  PropertyState state_;
  // Accepted commands awaiting hand-off, in request order.
  std::vector<CommandId> pending_;
};

}  // namespace talos
