# ADR 0013: Portable firmware, virtual board and ESP-IDF skeleton

**Status:** Accepted (2026-11-16). Implements Phase 5 (spec §32, §35, §36, §41). Supersedes the deferral in ADR 0005.

## Decision
- **`firmware/portable`** holds the device firmware core. The host build (Bazel, strict warnings) and the ESP32-H2 build (ESP-IDF CMake) compile the **same sources**. Style rules: no heap in hot paths, no exceptions, no RTTI, fixed-size values. Drivers live in a per-slot `std::variant`.
  - **HAL** (`hal.h`): clock, GPIO, I2C, power/rails, an atomic key-value store, A/B flash, and the link. Board wiring is a `BoardSupport` table: per-slot wake line, I2C mux branch and 5 V rail. Nothing is a hard-coded pin in portable code.
  - **Discovery:** each module has an ID EEPROM on its slot's mux branch (ID block v1: module code, revision, format, variant, CRC-8). Blank, corrupt and unreadable IDs are faults, distinct from an empty slot (PRESENT_N). A static registry maps codes to drivers and declares each module's needs (wake line, 5 V), whether it is critical, and its average power.
    - Unknown codes are never powered.
    - A missing resource, a failed probe or a broken branch is reported as a fault for that slot only; other slots keep working.
  - **Drivers:** PIR (edges), reed/Hall contact with **20 ms time-based debounce**, LIS2DW12 (WHO_AM_I probe, wake threshold, latched wake source, peak mg), SHT40 (command, CRC-8, conversion), and a smart-module presence driver for the camera. Register maps are provisional until validated on hardware.
  - **Runtime:**
    - **Boot:** a persistent boot epoch (never reuses `(epoch, seq)`), then restore config, the OTA record and unacknowledged critical events.
    - **Power:** discovery, then power admission. Critical sensing is always on. Optional loads (the camera's 5 V rail) run only when the active source's budget allows and the node is in normal power. Low battery stretches heartbeat and environment intervals ×4.
    - **Delivery:** a bounded 32-entry priority queue with exponential backoff (0.5 s to 8 s). P0 events are dropped last. Up to 8 unacknowledged P0 events are persisted and resent after a reset **with their original identity**, so the hub deduplicates them.
    - **Config:** `ConfigUpdate` uses requested/applied revisions. Stale revisions are ignored. Accepted values are persisted and acknowledged with `kConfigApplied`.
    - **Boot reports** of a contact's current state are marked as reports, not transitions. The hub sees `contact.state`, so a reboot never looks like a door opening. The twin and devsim follow the same rule.
  - **OTA** (`ota.h`), on the ESP-IDF A/B model:
    - offer → chunked download (resumable, in-order only) → streaming SHA-256 check → ready to boot → trial boot (pending verify) → confirmed on the first hub ACK, or rolled back if it reboots before confirming.
    - Downgrades, oversized images, bad digests, and starting while the battery is low are refused.
    - Every transition is persisted before it takes effect. A test cuts power at each of the first 40 store writes and checks the node always comes back on a complete image.
- **`firmware/sim`** is the virtual board (L2 twin). It has register-level models of the I2C mux, ID EEPROMs, LIS2DW12 (clear-on-read wake source driving INT1) and SHT40 (real CRC), plus GPIO, rails/battery, a store with power-cut injection, A/B flash and a lossy link. `hub_bridge` maps firmware events to `ObservationEnvelope`s, and the integration tests run the **real `HubEngine`** against the real firmware.
- **`firmware/esp_idf`** is the ESP32-H2 project (sdkconfig: 4 MB flash, two-OTA partition table, rollback enabled, no exceptions or RTTI). Clock, GPIO, I2C (`i2c_master`) and NVS adapters are real. Battery ADC, the `esp_ota` flash binding, sleep and wake interrupts, and the Thread/CoAP link are stubs: OTA offers are refused and events stay queued. CI builds it in the official `espressif/idf:v5.5.6` container. **The pins in `board_proto_a.h` are placeholders.**
- **SHA-256** moved to `//common/sha256`, a streaming implementation with no heap, shared by the hub (token hashes) and firmware (OTA).

## Not yet (honest limits)
- Release-manifest **signature** verification. Only the digest is checked today, and the digest comes from the offer. Owner-controlled signing keys are a P0 open decision (spec §36, §55).
- No physical timing: interrupt latency, sleep current, flash wear, brownout behavior and RF are HIL work (L4).
- The ESP-IDF build is **compile-verified only**. Nothing has run on silicon.
- Diagnostics events (module presence and faults, power mode, OTA status) are acknowledged but not yet ingested by the hub; that's the observability plane in Phase 6.

## Amendment 2026-11-20: failure-mode hardening (firmware review)
- **Safe expander state before anything turns on.** `BoardIo` starts its expander shadow at the board's safe pattern (every rail, RESET_N and CS_N off, UART mux parked). A rail or the UART mux can be turned on only after the expanders are configured, and configuration is retried on the next request. Off is always allowed. A failed `begin()` after a warm reset can no longer switch every slot on at once.
- **Queue loss policy:** a new event never evicts a higher-priority one. If it is lower priority than everything queued, the new event is dropped and counted. (A heartbeat used to be able to push out a P0 alarm.)
- **AON settle:** with the fitted 1 nF CT (TPS22917 turn-on ≈ 3.8 ms), card lines go high `kAonSettleMs` = 10 ms after AON is enabled. The virtual board models the rail rise and flags backfeed.
- **Task watchdog:** `IWatchdog` in the HAL. The ESP port subscribes the main task (5 s, panic then reset), and the runtime feeds it every step and between slot power-ups.
- **Slot quarantine:** two consecutive resets during a slot's power-on window (one could be a power cut) quarantine it. It is retried after 1 h of uptime, doubling per strike up to 24 h, and a successful power-up clears it. A remote clear needs a new downlink item and is not done; `ConfigUpdate` has no spare field.
- **Boot epoch:** the save is checked and retried, nothing is transmitted until it is persisted, and an exhausted epoch fails loudly. NVS is erased only on `NO_FREE_PAGES` or `NEW_VERSION_FOUND`.
- **Shared SPI pins** are parked at boot: GPIO4/5 driven low, no pull; MISO pulled down. Deep sleep still needs board pull-downs (HD-027; fitted in amendment (c)).
- **OTA trial images** are marked valid (`esp_ota_mark_app_valid_cancel_rollback`) only after a hub ACK *and* with the board's safe state reached. A node whose charger never answers rolls the update back.
- The portable library builds with `-fno-exceptions` on the host too (no-RTTI stays enforced by the ESP-IDF build; UBSan needs typeinfo on the host). OTA progress is persisted every 4 KiB, not every chunk.

## Amendment 2026-11-20 (b): red-team hardening
A red team of the node found 19 issues (alarm loss on a deep-sleep wake, NVS wear-out, battery drain during an outage, and more). Regression tests: `firmware/sim/redteam_test.cc` (its harness checks R1–R9, each failing on the old runtime) and `firmware/sim/supervision_test.cc`.
- **Deep-sleep wake = reset (ESP32-H2).** New HAL `ISleep`: reset cause, the ext1 wake-line mask, per-line wake enable, and LP RAM. The runtime keeps a 72-byte CRC-checked block there (epoch, seq, AON mask, per-slot module and driver state, ring ACKs, link state). It trusts the block only after a deep-sleep wake. On a wake it:
  - adopts the AON rails through `IPower::resume()` (`BoardIo` reads back the expander outputs and directions, and falls back to `begin()` on any mismatch);
  - drivers `resume()` instead of `init()`, and the waking slots run first. The TMAG5273 converts and compares with the state before the sleep. The LIS2DW12 reads its latched source *before* any reset. So the event that woke the node is a P0 transition or impact. The PIR stays warm.
  - writes no epoch, slot marker, module-present report or boot heartbeat. A software reset is still a cold boot.
  - Sleep entry itself is still not wired (`main.cc` light-polls). A trial OTA image must not deep-sleep before it is confirmed (HD-035).
- **NVS wear.**
  - Unacked P0s live in an 8-slot ring of ~44-byte records (`kPendingRing0..7`). An alarm costs one write; an ACK costs none. ACKs are written back at most every 5 min or before a planned reboot, and carried in LP RAM across wakes. A lost ACK only means the hub deduplicates the alarm once more.
  - The newest 8 alarms survive a reset, not the oldest.
  - Test: 0 writes for 10 acknowledged heartbeats, 1 write per motion cycle. That gives about 4 entries per alarm against the old 3 × 13, and the working NVS grows from 16 to 64 KiB (`partitions.csv`): at about 100k cycles, roughly 6 years of *continuous* motion (unverified: HD-035).
- **Identity and epoch** live in their own NVS partition, `talos_id`, which firmware never erases. Erasing the working partition no longer restarts the epoch (RT-06). Identity and epoch are never written to the erasable partition, and old nodes are read from it once.
- **Link health.**
  - After 8 unanswered sends and 30 s with no downlink, the link counts as failing. Then one probe goes out per interval, doubling from 10 s to 5 min, plus at most one per 30 s for a new P0. Any downlink ends it.
  - A token bucket caps all sends (burst 60, then one per 2 s on average; P0 bypasses it).
  - P2/P3 telemetry expires after 15 min, and only the newest heartbeat stays queued.
  - Downlink handling is bounded to 16 items per step.
  - The CoAP pending ring grew from 8 to 32 entries, so late ACKs for a full queue are accepted.
- **Supervision.**
  - A stuck wake line, card removal (PRESENT_N, or the ID EEPROM checked twice at each heartbeat), quarantine, a WAKE that never rose (TMAG/LIS latched with WAKE low: `kWakeLineFault`), and a TMAG that can't be re-armed (`kBusError`, b = `kFaultDetailUnarmed`) are sent as **P0** `kModuleFault` on critical slots.
  - New codes are `kCardRemoved` = 14 and `kWakeLineFault` = 15. The wire kind and layout are unchanged, so an old hub decodes them. The hub must treat a P0 module fault as a trouble condition.
  - Empty slots are rescanned at each heartbeat (hot insertion).
  - A wake line high for 30 s is masked as a wake source and polled each second until it falls. It is reported as stuck after 10 min, unless it is a contact held open.
- **Drivers.** A TMAG5273 that fails mid-poll retries every 50 ms, up to 10 times. It is then parked in sleep and retried every minute. At each heartbeat it is checked to still be in wake-and-sleep. PIR warm-up ends at 210 s if the output is still toggling (continuous motion), not only once it rests.
- **OTA.** A newer offer replaces an unfinished download, and so does any offer after 6 h without progress. The slot is erased block by block, and the watchdog is fed per block and every 4 KiB of hashing.
- **Config.** Revision 0xFFFFFFFF is rejected (a stored one is repaired at boot). Heartbeat is capped at 1 h and environment at 1 day, so `× 4` can't wrap.
- **Not done:** gating the destructive diag commands on the USB console (RT-13, HD-016); batching several events per uplink frame; a hub-side "re-epoch" reply. Enclosure tamper, magnet tamper and wake-line hardware are HD-032 to HD-035 (HD-032 and HD-034: see amendment (c)).

## Amendment 2026-11-20 (c): Rev A board review (owner decisions)
The owner took the board good-practice review (hardware/requirements/board-practices-review.md). Firmware, contracts and tools follow it; nothing has run on a board.
- **Third expander EXP_C** (TCA9535, 0x22): P00–P03 slot LEDs S0–S3 (active low), P04 `TAMPER_N`, everything else unconnected and driven low at boot. `BoardSupport` gains `slot_led_n`, `tamper_n`, `unused_pins`, `expander_wake_line` and `status_led_line`; `BoardIo` handles three expanders. `resume()` ignores the LED pins, so a lit LED never makes a deep-sleep wake cut the rails.
- **EXP_WAKE (HD-034).** `EXP_INT_N` now reaches GPIO14 through an inverter, active high, so it joins the slot WAKE lines in one ext1 `ANY_HIGH` mask. When the line is high, the runtime reads every expander port (`IPower::service_interrupt()`), which releases INT. A line that stays high is masked after 30 s and polled every second, like a stuck slot WAKE. A presence probe re-reads its port, so its own edge never leaves INT asserted.
- **Tamper (HD-032).** New `EventKind::kTamper` = 10, appended (older kinds keep their numbers; the hub's wire decoder accepts it). Debounce is 50 ms, from the injected clock. Opening is P0, a restore is P1, and a cold boot sends a state report (b = 1). A wake from EXP_WAKE reads TAMPER_N at boot. The state before the sleep is kept in LP RAM (the `flags` byte, so the layout does not change), so a lid opened during sleep is a P0 transition. The hub shows the node FAULTED while the lid is open. The CoAP bridge turns it into a `tamper.enclosure` observation, so it reaches the journal and rules (ADR 0007 amendment 2026-11-20). The board's RC is now 1 MΩ / 10 nF (τ = 10 ms), well inside the 50 ms debounce.
- **Indicators** (`indicators.h`, pure and tested). D1 shows one blink code at a time, by priority: fault, boot, card error, OTA, low battery, joining, charging (external power only), joined. Every pulse is 20 ms, so the duty cycle is ≤ 2 %. Slot LEDs are dark on battery. On USB or PV, or for 10 min after `led service on`, they show solid (card running), slow blink (fault or quarantine), or a 150 ms inverted flash on a wake event. Wake times include the next LED edge, computed when the LEDs were last updated.
- **Board identity (D-09).** `StoreKey::kBoardInfo` lives in `talos_id` (permanent). Diag `board-id <board> <rev> <serial>` writes it once and refuses changes. `talos_dbg program-id --node` writes it and reads it back, and bench S-M16 checks for "TL-SN-001" rev "A".
- **sensor_edge_v1 (HD-025):** PRESENT_N is on contact 19, and contact 18 is reserved. **SPI (HD-027):** the board now has 100 kΩ pull-downs on SCK/MOSI/MISO; firmware still parks the pins at boot.

## Amendment 2026-11-21: service mode from the BOOT button
- `BoardSupport::service_button_line` (TL-SN-001: GPIO9, BOOT_N, active low with a pull-up). A debounced press (30 ms, injected clock) toggles the 10-minute slot-LED service mode; the timeout also ends it. The press counts only after the button was seen released, so a button held while the app starts does nothing.
- GPIO9 is a strap pin. The ROM samples it at reset and enters download mode before the app runs. The app configures it only afterwards, as an input with a pull-up, and never drives it, so neither the strap nor download mode is affected.

