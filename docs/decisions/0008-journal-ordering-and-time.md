# ADR 0008: Journal ordering, deduplication and time

**Status:** Accepted (2026-11-15). Implements spec §21-22. This is the foundation for replay and the digital twin.

## Three different "times"
| Field | Set by | Meaning | Used for |
|---|---|---|---|
| `journal_seq` | hub, at append | Ingestion order, strictly increasing, persisted | **The only cross-device ordering authority.** Replay and reduce iterate by it. |
| `received_at` | hub clock, at append | When the hub accepted the entry | "State at time *t*", retention, UI timelines |
| `observed_at` | device clock | When the device thinks it happened; may be wrong or absent | Display and diagnostics only. **Never used for ordering.** |

`received_at` is kept **non-decreasing in journal order**. If the hub clock steps backwards, the entry keeps the previous `received_at` and is flagged `clock_regressed`. This makes `state_at(t)` (all entries with `received_at <= t`) always a clean journal prefix. Freshness fails closed after such a step: a capability whose `received_at` is ahead of the current hub time reads as **stale**, because its true age is unknown (`StateTest.ReceiptAheadOfTheClockIsStaleNotFresh`).

## Source positions and dedup
- Each device reports `(boot_epoch, sequence)`. `boot_epoch` is a persistent counter incremented on every boot, and `sequence` restarts at each boot. This identifies an event **at its source** and orders events **only within one device**.
- Deduplication is keyed by `(device, boot_epoch, sequence)` with bounded memory. For each (device, boot) the hub keeps the highest sequence plus a 1024-entry window of which recent sequences arrived, and it remembers the newest 8 boot epochs per device.
- A sequence older than the window **cannot be proven unique** and is refused as `too_old`. This trades a rare lost late event for never causing a duplicate effect (invariant 4). The window size is a tunable to revisit with measured queue depths.
- Each accepted observation records its `SourceOrder` relative to what the hub had seen: `in_order`, `gap`, `late` or `new_boot`. Gaps are visible for diagnostics.
- Dedup state is rebuilt from the journal on restart (`Ingestor::restore`).

## Projection rule
For each capability, current observed state keeps the value with the **highest source position** from that device: boot epoch first, then sequence. A late (reordered) observation is journaled and visible in history, but it does not overwrite a newer value. Together with `journal_seq` ordering, this makes the final state independent of how independent devices' deliveries interleave. A seeded metamorphic test covers this.

## Amendment (2026-11-20, red team): bounded positions and owner reset
One forged report with boot epoch `0xFFFFFFFF` used to hold every capability of a device forever, across restarts and key rotations: every later genuine report was "late". Now:
- **Bounds.** A sequence is at most `UINT32_MAX` (the wire width) and at most 2^20 past the highest seen in its boot. A new boot epoch is at most 64 past the newest one seen. A device with no history accepts epochs up to 2^20. The caller may raise the epoch limit with a ceiling: the HTTP device API passes hub time + 300 s, because HTTP adapters (devsim, Frigate) use Unix-second epochs; the CoAP path passes none. Anything beyond is refused as `source_position_out_of_range`.
- **Health.** Refused jumps, and accepted reports from an older boot than the held position, mark the device `FAULTED` ("reports ignored as late") in `/v1/health/devices` for the offline window, instead of `HEALTHY`.
- **Recovery.** `POST /v1/devices/{id}/sequence/reset` (owner only) journals a `SourceReset`. Replay and restore drop the device's dedup windows, the held positions of its capabilities fall to zero (values stay, still subject to freshness), and pending commands' dispatch baselines for it are cleared. The next genuine report becomes current again. Because the dedup window for that device is reopened, rotate the device's key at the same time, so old frames cannot be replayed.
- **Device contract** (CoAP firmware): `boot_epoch` is a persistent counter incremented by exactly 1 per boot, never time-based; `sequence` restarts at each boot, increments by 1 per event and stays within 32 bits. A device that boots more than 64 times without one accepted report, or that is reflashed with a reset counter, needs an owner reset. HTTP adapters may use Unix-second epochs. See ADR 0014.

## Snapshots
A snapshot is a `PropertyState` with `through_seq`. `replay(snapshot, journal)` applies only entries after `through_seq`, and it is tested to equal a full replay. Reducing an entry at or below `through_seq` is a no-op.

## Unknown and stale
A capability with no report is `unknown`, never "clear" or "healthy". A report older than its `max_age` is `stale` (invariant 3).
