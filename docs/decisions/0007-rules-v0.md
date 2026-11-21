# ADR 0007: Rule engine v0 semantics

**Status:** Accepted (2026-11-15). Partial implementation of spec §23's automation DSL.

## Decision
- A rule has a trigger (`event_type` plus an optional zone), conditions (allowed arm modes, plus an optional local time window that may wrap midnight) and typed actions.
- Zone resolution uses `Property::zone_of`. An attached asset's zone wins over the device's placement.
- `evaluate_rules` is pure. Each action becomes an `Intent` with the deterministic key `rule_id@revision:causation_event_id:action_index`. That key becomes the command's `idempotency_key`, so re-evaluating the same event can never produce a second command. A new rule revision is a distinct decision.
- Rules act under their own principal (`PrincipalKind::kRule`) and pass through policy like everything else.
- Time-zone and DST conversion happens outside core. Callers pass the property-local minute of day.

## Deferred
Cooldowns, priorities and conflict resolution, sun-relative schedules, dry-run against history, and a user-facing YAML/JSON rule format (`rule.proto` exists; codec mapping comes with the Phase 3 API).

## Amendment 2026-11-20: enclosure tamper is an alarm path
- A node's enclosure tamper loop (HD-032) is an observation, not just a health flag. Capability `tamper.enclosure` (device-level, no slot), payload `TamperState{open}`, event types `tamper.opened`, `tamper.closed` and `tamper.state` (a boot state report). Ingest enforces them like every other kind: the device must declare the capability, the payload must fit it, and the event type must match the value. Dedup is by (boot_epoch, sequence), and cross-device order comes from journal_seq. The CoAP bridge maps firmware `EventKind::kTamper` to it (b = 1 → `tamper.state`).
- **Decision: tamper alarms in every arm mode** (24-hour zone, the alarm-panel convention). The rule engine is unchanged. A rule with empty `armed_modes` already matches in every mode, so tamper rules are written without them, as the dev config's `enclosure_tamper_alarm` is. Opening the lid while disarmed must still raise the alarm, because disarming must not hide someone taking the sensors apart.
- Only a transition alarms. A `tamper.state` report of an open lid, sent when a node cold-boots with the lid off, is journaled and shown (the device is FAULTED and the UI shows the enclosure open) but triggers no rule, just as `contact.state` doesn't. Closing is journaled too (`tamper.closed`).
- Health: the hub shows a device FAULTED while its last tamper state is open, however old that report is. An open lid never ages into healthy. The UI shows it as a fault, and never-reported or stale-closed tamper as unknown (ADR 0011).
- Boards declare the capability through `board_capabilities` in their manifest (TL-SN-001: J9 lid loop).

## Amendment 2026-11-21: maintenance mode for tamper
Because tamper alarms in every mode, every battery or card change would trip it. The owner decided on a bounded, per-node maintenance window:
- **Operation `device.maintenance`** goes through the ordinary command path (catalog → policy → ledger). It takes `SetMaintenance{duration_ms}` (0 means 30 min; allowed range 1 min to 4 h) and targets the node's `device.health`.
  - It is security-critical, so only humans with a security_critical grant can start it. AI agents never can, whatever their grant (`kMaxNonHumanRisk`).
  - The node must be reporting (fresh heartbeat).
- **`device.maintenance_end`** is benign, because ending maintenance raises protection, like arming.
- Both operations are **hub-internal** (`OperationSpec::hub_internal`). The engine journals the effect and then marks the command dispatched and verified; the journaled state change is the evidence. They are never handed to a device.
- **Journal:**
  - `MaintenanceChanged{device, until | none, principal, reason started/cancelled/expired, command}` records each start and end.
  - While a window is open (evaluated on the injected clock), a `tamper.opened` from that node is journaled as usual. Every rule it would fire is replaced by `AlarmSuppressed{device, rule, causation, until, principal}`.
  - Other events from the node, and tamper from other nodes, are unaffected.
- **Ending:** `tick()` ends expired windows (principal `system:maintenance_expiry`). If, at expiry or at a cancel, the node's last tamper state is still open, the rules are evaluated again for that opening, under the same event id. A suppressed opening alarms then. An opening that had already alarmed before maintenance began dedupes to the command it made.
- **Visibility:** health shows MAINTENANCE (never healthy) with the time left, and `StateResponse.maintenance` lists open windows for the UI.

## Amendment 2026-11-21 (b): tamper and maintenance red team
A red/blue review of the two amendments above found these, now closed (tests named in REQ-038/REQ-042):
- **Late openings alarm.** The node's P0 opening can sit in retry backoff while its P1 restore overtakes it. The opening then arrives superseded (ADR 0008) and, as history, fired nothing, so a lid opened and closed again went unannounced. A superseded `tamper.opened` now still runs the rules (maintenance suppression applies; it verifies nothing and does not change current state). This is the one exception to "history never triggers".
- **A cold-boot state report can be an opening.** Power loss, a watchdog reset or a brown-out between the lid opening and the P0 leaving loses the transition; after the boot the node can only send `tamper.state`. If the hub last knew the lid *closed*, a `tamper.state` open alarms as an opening, under the report's own event id. If the hub never knew the state (commissioning), it doesn't alarm; the device shows FAULTED.
- **Event ids belong to their source.** Rule command keys and the maintenance re-evaluation are keyed on `event_id`. Ingest now refuses an observation whose `event_id` does not start with `<source device>:` (`event_id_not_owned_by_source`), so one device can't pre-claim another's next id and make that device's alarm dedupe to a command already spent. All producers (CoAP bridge, devsim, twin, Frigate) already namespace their ids.
- **Maintenance chains are capped.** A `device.maintenance` while a window is open keeps the window's start (`MaintenanceWindow::since`, from the journal's received time, so it replays). One that would end more than `kMaxMaintenance` (4 h) after that start is denied `maintenance_limit`. So an open lid alarms at most 4 h after the first start, however often the window is restarted. After a window ends (and alarms if the lid is open), a new one starts a new chain.
- **Maintenance covers the tamper alarm only, also in health.** Health no longer returns MAINTENANCE before looking at slot faults, power and OTA; those still show, with the maintenance noted. OFFLINE still wins and names an enclosure last reported open. The web UI's device row lets maintenance replace only a healthy tone, never offline, unknown, stale or a fault.
- **Hub-internal commands never reach a device.** A crash between accepting `device.maintenance` and journaling its effect left it accepted; on restart it was queued and handed to the node. Hub-internal commands are now never queued or handed off; such a command expires by its TTL.
- **Firmware chatter.** The tamper debouncer restarted on every bounce, so a loop chattered faster than 50 ms (a reed switch pulsed by a magnet) was never reported. It now fails toward open: once the loop opens, only a closed level held for 50 ms cancels the opening; the restore stays symmetric.
- Accepted risks are in `open-decisions.md` ("Tamper/maintenance residual risks").
