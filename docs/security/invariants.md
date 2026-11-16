# Security invariants

From spec §45. Each invariant is mapped to the test that enforces it today, or to the phase that will.

| # | Invariant | Enforced by |
|---|---|---|
| 1 | High-risk physical operations go through authenticated, authorized, time-bounded local policy | `//core/policy:policy_test` (deny by default, risk ceilings, human-only security-critical, freshness); `//core/execution:ledger_test` (TTL) |
| 2 | Cloud outage cannot disable local sensing and alarms | Architectural: core has no network dependency. Scenario test in Phase 2/3 |
| 3 | Missing or stale telemetry never reports a sensor healthy or clear | `StateTest.FreshnessNeverReportsUnknownAsFresh`; `Policy.SecurityCriticalRequiresFreshTargetState` |
| 4 | Replayed events and duplicate commands cannot cause repeated physical effects | `IngestTest.DuplicateDeliveryHasNoEffect`, `IngestTest.RefusesSequencesOlderThanWindow`, `Ledger.SameIdempotencyKeyYieldsOneCommand`, `Rules.ReEvaluatingSameEventYieldsSameIntentKey`, `Phase1Slice.DuplicateMotionDeliveryCausesOnePhysicalEffect` |
| 5 | Security-critical remote actions expire and need fresh state and authorization | `Ledger.ExpiredCommandIsNeverDispatched`, `Policy.ExpiredRequestDenied`, `Policy.SecurityCriticalRequiresFreshTargetState` |
| 6 | A failed device cannot be misreported as having acted successfully | `Ledger.AckWithoutObservationIsCompletedUnverifiedNeverSuccess`, `Ledger.NoEvidenceInWindowIsUnknownNotSuccess`, `Phase1Slice.SpeakerSilenceEndsUnknownNotSuccess`; web `outcomeOf` test |
| 7 | Only verified, signed, compatible OTA images are accepted | Phase 5/6 (OTA model) |
| 8 | Untrusted firmware or module profiles cannot change wiring or current limits by remote manifest | Board manifests are build artifacts; nothing in core mutates them. Enforcement tests in Phase 5 |
| 9 | No private credentials or signing keys in the repo, factory logs or client bundle | Process + secret scanning in CI (to add with release workflow) |
| 10 | Users and agents act only within authorized property and capability scopes | `Policy.GrantForOtherPropertyDoesNotApply`, `IngestTest.RejectsForeignPropertyAndUnknownDevice`, `ValidateProperty.*CrossProperty*` |
| 11 | A hardware safety interlock cannot be overridden by an automation rule | Hardware design (Phase 8); policy caps rule principals today |
| 12 | Telemetry floods cannot exhaust critical event and control queues | Phase 3/6 (priority queues, rate limits) |
