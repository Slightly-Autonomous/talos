# ADR 0004: Hub authority and command lifecycle semantics

**Status:** Accepted (2026-11-15). Implements spec D02, D21, §23 and security invariants 1, 4, 5 and 6.

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
- Verification has a separate window: `dispatched_at + verification_window`.
- `unknown_result` means the command was dispatched and **no ACK and no confirming observation** arrived in the window.
- `completed_unverified` means an ACK arrived (or the mode is `ack_only`) but no independent observation confirmed the state.
- **Late evidence is recorded faithfully**, with `late=true`. A late ACK moves `unknown_result` to `device_acknowledged` and then `completed_unverified`. A late NACK moves it to `rejected`. A late matching observation moves it to `verified`. The ledger never hides a real outcome behind `unknown_result`.
- An ACK for a command that was never dispatched is reported as an error (`ack_for_undispatched_command`) and changes nothing.

### Success
Only `verified` counts as success (`outcome_of`). `completed_unverified` is reported as *unverified* and is never shown as done (invariant 6).

### Idempotency
A second request with the same `idempotency_key` returns the existing command and produces no transitions. Duplicate and out-of-order ACKs are no-ops.

### Policy (core/policy)
- Deny by default. A principal needs a grant for the command's property, up to a risk ceiling.
- The risk class comes from the operation catalog, **never from the caller**. Unknown operations are treated as `security_critical`.
- Non-human principals (agents, rules) are capped at `disruptive_output`, whatever their grants say.
- `security_critical` operations require the target's state to be fresh. Unknown freshness counts as not fresh.

## Spec deviations
The spec's `queued` and `timed_out` states are not implemented. Sleepy-device queuing waits for Phase 6. `timed_out` was ambiguous with `unknown_result`, so it was folded into it. Either can be added later without breaking the ledger.
