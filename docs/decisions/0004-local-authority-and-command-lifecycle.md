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
- the target capability is declared by that device, when the device declares capabilities (`undeclared_capability`);
- freshness, always on the **target**.

`ack_only` commands never carry an `expect`, and the ledger ignores observations for them. Rule-originated commands must run under a `kRule` principal (`rule_principal_must_be_rule_kind`).

## Spec deviations
The spec's `queued` and `timed_out` states are not implemented. Sleepy-device queuing waits for Phase 6. `timed_out` was ambiguous with `unknown_result`, so it was folded into it. Either can be added later without breaking the ledger.
