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
