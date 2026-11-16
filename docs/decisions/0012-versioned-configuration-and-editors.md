# ADR 0012: Versioned configuration, property editor and rule editor

**Status:** Accepted (2026-11-16). Implements Phase 4b (spec §21 layout revisions, §23 rule dry-run, §28 stories 2 and 5).

## Context
Phase 4 shipped read-only views. Editing the floor plan, devices and rules from the website requires:
1. A config write path that goes through authorization and is audited.
2. History that keeps referring to the layout and rules **in force when each event happened**. Correcting a floor plan must never rewrite the past.
3. A way to preview a rule against real history before enabling it.

## Decision
- **Config document:** a `talos.server.v1.ConfigDocument` holds the property, with geometry, and the rules. It's the editable part of the hub config. Principals, credentials, storage and network settings stay in the YAML file and are never editable over the API.
- **Revisions:** each accepted change becomes a new revision stored in the hub's SQLite file (`config_revisions`: revision, created_at, principal, sha256, document bytes). The journal also records a `ConfigRevisionApplied {revision, sha256, principal}` event. The journal stays the single ordering authority, so "which revision applied at time t" is answered by replaying the journal prefix.
- **Bootstrap:** if no revision exists, the YAML's property and rules become revision 1 at first start.
- **Write API:** `PUT /v1/config` with `{base_revision, document}`.
  - Optimistic concurrency: a stale base gets 409 Conflict.
  - The document is validated exactly like startup config: property integrity, rules target this property, and no unknown devices in rules.
  - Authorization: changing config is **security-critical**. It needs a human principal with a security-critical grant, through `core/policy` (`authorize_config_change`).
  - The engine is reconfigured in place. State is untouched, because state derives from the journal, not from config.
- **Read API:** `GET /v1/config` (current) and `GET /v1/config?at=<unix_ms>` (revision in force at that time). Replay draws the floor plan of that time.
- **Rule preview:** `POST /v1/rules/preview {rule, from_ms, to_ms}` dry-runs one rule over historical observations. It uses the pure core `evaluate_rules`, with the arm mode in force at each observation. It returns would-fire matches and dispatches nothing. Because it runs the same matcher the engine uses, it can never drift from the engine.
- **Web editors:**
  - **Property editor:** zones as polygons edited vertex by vertex on a metric grid, devices placed by click, door/window segments, names and kinds.
  - **Rule editor:** trigger, zone, arm modes, schedule, speak/notify actions, plus a "Preview against last 24 h" button.
  - Both save through `PUT /v1/config`, showing a diff summary and the conflict message when someone else changed config first.

## Consequences
- Events are interpreted against the revision active when they were journaled, and replay shows the old layout.
- The hub keeps working if the web editor is broken: config can always be fixed in the YAML bootstrap plus a reset, or through the API.

## Deferred
Floor-plan image/PDF import and scale calibration, multi-level buildings, coverage editing UI, and approval workflows with a second person.
