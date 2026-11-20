# ADR 0004: Hub authority and command lifecycle semantics

**Status:** Accepted (2026-11-15), amended 2026-11-20 (operation catalog, dispatch at hand-off, hub-derived expectations; see "Amendment"). Implements spec D02, D21, §23 and security invariants 1, 4, 5 and 6.

## Decision
The local hub is the only authority over commands. Web, cloud and AI clients submit intents. They never write ledger state.

### Ledger as a pure state machine
`core/execution/CommandLedger` has decision methods (`request`, `dispatch`, `on_ack`, `on_observation`, `tick`, `cancel`) that **return transitions without mutating anything**. The caller journals each transition and then calls `apply()`. Replaying journaled transitions rebuilds an identical ledger, including the idempotency index.

### Lifecycle
```
requested ─┬─> denied                                   (policy refused)
           ├─> expired                                  (already past TTL)
           └─> accepted ─┬─> expired                    (TTL elapsed before dispatch: never sent)
                         ├─> canceled
                         └─> dispatched ─┬─> device_acknowledged ─┬─> verified
                                         │                        └─> completed_unverified (window closed)
                                         ├─> verified           (observation before ACK)
                                         ├─> rejected
                                         └─> unknown_result     (window closed, no evidence)
```

### Expiry is not verification
- `expires_at` (TTL) **only gates dispatch.** An expired command is never sent.
- **Dispatch is the hand-off to the device adapter**, not the moment of acceptance. An accepted command waits in the engine until its target's adapter takes it (`HubEngine::take_outbound(device)`, used by `GET /v1/devices/commands`). That call runs `ledger.dispatch()`, which expires the command instead if its TTL has passed. So a command queued while an adapter is offline is never delivered late.
- Verification has a separate window: `dispatched_at + verification_window`.
- `unknown_result` means the command was dispatched and **no ACK and no confirming observation** arrived in the window.
- `completed_unverified` means an ACK arrived (or the mode is `ack_only`) but no independent observation confirmed the state.
- **Late evidence is recorded faithfully**, with `late=true`. A late ACK moves `unknown_result` to `device_acknowledged` and then `completed_unverified`. A late NACK moves it to `rejected`. A late matching observation moves it to `verified`. The ledger never hides a real outcome behind `unknown_result`.
- An ACK for a command that was never dispatched is reported as an error (`ack_for_undispatched_command`) and changes nothing.

### Success
Only `verified` counts as success (`outcome_of`). `completed_unverified` is reported as *unverified* and is never shown as done (invariant 6).

### Idempotency
A second request from the **same principal** with the same `idempotency_key` (or command id) returns the existing command and produces no transitions. Keys are scoped per principal, so one principal's key never matches another's command. Reusing another principal's command id is a `conflict` (HTTP 409) that reveals nothing about that command. Duplicate and out-of-order ACKs are no-ops.

### Policy (core/policy)
- Deny by default. A principal needs a grant for the command's property, up to a risk ceiling.
- The risk class comes from the operation catalog, **never from the caller**. Unknown operations are treated as `security_critical`.
- Non-human principals (agents, rules) are capped at `disruptive_output`, whatever their grants say.
- `security_critical` operations require the target's state to be fresh. Unknown freshness counts as not fresh.

## Amendment (2026-11-20): the operation catalog is the single source of truth
A review showed that devices act on `args`, while policy judged only the caller's `operation` string. It also showed that the caller chose `expect`, and that `expect` decided both freshness and verification. `core/execution/operations.h` now defines, for each operation:
- the one `CommandArgs` alternative it takes;
- the target capability id it must address;
- its risk class (`risk_class_for` reads the catalog);
- its longest TTL: 30 s for `security_critical`, 5 min otherwise. The verification window is at most 5 min;
- its expected observation, **derived on the hub** from args and target. For example, `access.set_locked{locked:x}` expects `lock{locked:x}` from the target.

`HubEngine` runs `normalize_command` before policy. A mismatch is denied with a stable reason: `unsupported_operation`, `args_do_not_match_operation`, `target_does_not_match_operation`, `expectation_does_not_match_operation`, `ttl_out_of_bounds` or `verification_window_out_of_bounds`. A caller-supplied `expect` is accepted only when it equals the derived one. The accepted command is journaled with the derived `expect`. The engine then checks:
- the target is a property device (`unknown_target_device`);
- the target capability is declared by that device (`undeclared_capability`). An empty capability list declares nothing (fail closed, 2026-11-20); `device.health` is implied for every device;
- freshness, always on the **target**.

`ack_only` commands never carry an `expect`, and the ledger ignores observations for them. Rule-originated commands must run under a `kRule` principal (`rule_principal_must_be_rule_kind`).

## Amendment (2026-11-20, red team): evidence must be current, ids are not claimable
- **Only current state verifies.** An observation verifies a command only if it became the target capability's current state, i.e. it has the highest source position the hub holds for that capability (ADR 0008 projection rule). A late, reordered report is journaled as history but is never evidence, so `verified` always agrees with the state the UI shows. In addition, the reducer records the target's source position at dispatch (`CommandRecord.baseline`, derived, not journaled), and only a strictly newer report from the target verifies. Both are per-device comparisons, so they are consistent with `journal_seq` being the only cross-device order. (`EngineTest.OutOfOrderOlderObservationNeverVerifies`, `EngineTest.ReportNotNewerThanDispatchBaselineNeverVerifies`, `HttpE2E.StaleObservationNeverVerifiesALockCommand`.)
- **Rule command ids are reserved.** Rule commands use `cmd:<rule>@<rev>:<event_id>:<i>`; the API refuses caller ids that start with `cmd:`. Event ids are bounded and HTTP event ids must start with the device id. If a rule's id is ever held by another principal anyway (an old journal), the action is recorded under `…:conflict-<journal_seq>` (same idempotency key) and counted in `talos_rule_command_conflicts_total`; it is never dropped.
- **Denied requests do not hold ids.** A denied request is kept apart from the id namespace, keyed by (principal, id), and only the newest 256 are kept in memory. Another principal can then use the id. The hub rate-limits command requests per principal (5/s, burst 20, then 429), and journals denials only within a per-principal audit budget (burst 20, one per 5 s); beyond that they are answered and counted (`talos_command_denials_unrecorded_total`), not recorded.
- **Free text is bounded:** `audio.speak` text at most 280 bytes and volume 0-100; `notify` text at most 1024 bytes (`args_out_of_bounds`).
- **Arming is ordered by protection** (disarmed < armed_home < armed_away). Lowering it, including away → home (which turns away-only rules off), is a disarm for policy purposes: human principals with a security-critical grant only. Agents and other non-humans may only raise it.

## Spec deviations
The spec's `queued` and `timed_out` states are not implemented. Sleepy-device queuing waits for Phase 6. `timed_out` was ambiguous with `unknown_result`, so it was folded into it. Either can be added later without breaking the ledger.
