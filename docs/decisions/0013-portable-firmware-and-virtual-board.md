# ADR 0013: Portable firmware, virtual board and ESP-IDF skeleton

**Status:** Accepted (2026-11-16). Implements Phase 5 (spec §32, §35, §36, §41). Supersedes the deferral in ADR 0005.

## Decision
- **`firmware/portable`** holds the device firmware core. The host build (Bazel, strict warnings) and the ESP32-H2 build (ESP-IDF CMake) compile the **same sources**. Style rules: no heap in hot paths, no exceptions, no RTTI, fixed-size values. Drivers live in a per-slot `std::variant`.
  - **HAL** (`hal.h`): clock, GPIO, I2C, power/rails, an atomic key-value store, A/B flash, and the link. Board wiring is a `BoardSupport` table: per-slot wake line, I2C mux branch and 5 V rail. Nothing is a hard-coded pin in portable code.
  - **Discovery:** each module has an ID EEPROM on its slot's mux branch (module code + revision). A static registry maps codes to drivers and declares each module's needs (wake line, 5 V), whether it is critical, and its average power.
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
