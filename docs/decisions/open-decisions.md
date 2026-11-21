# Open decisions and risk register

A living register (spec §55). Nothing here is decided until an ADR, a test, a benchmark or a hardware experiment settles it. **Hardware numbers in `contracts/manifests/` are placeholders until measured.** The detailed hardware register (HD-001..HD-019, with evidence and next actions) is [`hardware/requirements/design-risks.md`](../../hardware/requirements/design-risks.md).

| Pri | Topic | Open question / risk | How it gets decided |
|---|---|---|---|
| P0 | Sensor connector | Series chosen 2026-11-17: Samtec HSEC8-110-01-S-D-EM2, 20 contacts, 30 µin gold (hardware HW-0007). Land pattern, host locating slot and card tongue now follow Samtec's drawing. Still open: check two drawing-read dimensions on a sample, mating durability, enclosure clamping against fretting | Samples + mechanical DFM |
| P0 | GPIO/pin map | Exact ESP32-H2 module and pin mux, boot/flash/USB, deep-sleep wake on four slots | Datasheet + board validator + bench tests |
| P0 | Mainboard power | USB/PV ORing, charger (BQ25186 is a candidate only), 1S pack/NTC, rails, per-slot protection | Schematic, lab stress and thermal tests |
| P0 | Security trust | Owner-controlled signing, provisioning, eFuse flow, recovery, unique identities | Threat model + ESP-IDF reference + HIL |
| P0 | Camera performance | Decided: separate Vision Node, no camera cartridge (HW-0001). Open: compute/codec, night/IR, pre-trigger buffering, real power | H5 camera benchmark |
| P0 | OTA resources | Is 4 MB flash enough? Partition sizes; update over Thread | Real images + HIL |
| P0 | Actuation safety | Strong authorization, local fail-safe, no policy bypass paths | Security invariants + hardware interlocks |
| P1 | Postgres driver | libpq via Bazel for `STORAGE_DRIVER_POSTGRES` (cloud-hosted hub on Supabase/Postgres) | Implement behind `IJournal` + conformance suite |
| P1 | Remote access | Relay role, per-principal read scopes (Supabase JWT auth is done: ADR 0015) | Remote-access phase (ADR 0010) |

| P1 | Sensor specifics | Hall vs reed, accelerometer thresholds, PIR model | Prototype BOM, measured false alarms and power |
| P1 | Power budget | Real sleepy-node average, charger quiescent, pack life, panel size | Profiler measurements + weather scenarios |
| P1 | Four-slot modules | Per-slot ID EEPROM, duplicate I²C, multi-wake modules, UART conflicts | Electrical prototype + manifest tests |
| P1 | Tamper loop supervision | Tamper is now an alarm path (ADR 0007 amendment 2026-11-20). Still open: the single-ended J9 loop reads closed when shorted to GND. An end-of-line resistor (closed / open / shorted / cut) needs an ADC, and none is free. The wall-removal switch is wired in series, so it cannot be told apart from the lid | Next board revision: a supervised loop on an ADC pin, or accept the limitation |
| P1 | Tamper/maintenance residual risks | Accepted after the 2026-11-21 red team (ADR 0007 amendment (b)): (1) an owner who starts maintenance after a lid opened but before its report arrives (node asleep or offline) suppresses that opening; if the lid is closed before the window ends, nothing alarms (humans only, journaled as `AlarmSuppressed`). (2) A first-ever `tamper.state` open (hub never knew the state, e.g. commissioning) does not alarm; the device shows FAULTED. (3) A flood of tamper openings from one node makes one alarm command each: no cooldown yet (ADR 0007 deferred). (4) Late openings of other kinds (contact, motion) are still history only, exactly the P0-overtaken-by-P1 loss fixed for tamper; extending the exception is a rules decision. (5) Event-id ownership is a prefix check: a device id that is another's prefix plus ":" (e.g. `a` and `a:b`) could still share ids; device ids are owner-authored. (6) Toggling external power (PV cable) can hold EXP_WAKE asserted; the node then polls at 1 Hz (detection still within ~1 s) at a battery cost. (7) The BOOT service button and its LEDs are inside the enclosure, so using them means the lid is open (alarm); service mode is bounded to 10 min and shows only card state. (8) The hub's maintenance expiry follows the hub's wall clock; a clock step back lengthens a window. (9) The UI's remaining-time label uses the browser clock; the dot uses the hub's health verdict | Revisit with rule cooldowns, provisioning (unique device ids) and the supervised loop |
| P0 | Device provisioning | Claim/commissioning flow that generates per-device CoAP keys (today a YAML file, ADR 0014) | Spec §37 design + HIL |
| P1 | Thread validation | OTNS/OpenThread POSIX harness for partitions, parent switching, sleepy polling | Linux CI job with OTNS |
| P1 | Dedup window | Is 1024 sequences per (device, boot) enough given device-side queue depth? (ADR 0008) | Firmware queue design, Phase 5/6 |
| P1 | Uplink freshness | Bounds (epoch +64, sequence +2^20) and owner reset limit epoch poisoning; an authenticated monotonic uplink counter or a proof-carrying new epoch would remove the need for a reset (ADR 0008/0014 amendments) | Firmware wire-format change |
| P1 | API pagination | `/v1/state` returns every non-denied command; paginate or window it (red team #8) | Measured history sizes |
| P2 | Device credentials | Devices hold both an HTTP token and a CoAP key; CoAP-only nodes could drop the token (red team #4) | Provisioning flow (spec §37) |
| P1 | NVR | Frigate vs custom adapter, RTSP/ONVIF profiles | Integration proof of concept |
| P1 | Hub hardware | Pi model vs mini-PC; MQTT placement; RCP firmware | Measured load |
| P1 | Twin calibration | All PIR/camera/impact/latency parameters in `simulation/scenarios/sites` are placeholders (ADR 0009) | Bench and field measurements, Phase 8 |
| P2 | Config editing | Floor-plan image import and scale calibration, multi-level, coverage editing, two-person approval (ADR 0012) | Phase 4c / user feedback |
| P1 | Rule complexity | Cooldowns, priorities, conflicts, sun schedules (ADR 0007) | Scenario coverage |
| P2 | Observability stack | Default Compose profile, retention overhead | Compose performance tests |
| P2 | AI/MCP connector | Client support, OAuth, required human confirmations | Client verification + policy tests |
| P2 | Certification | RF/EMC/battery/product safety | Professional pre-compliance |
| P2 | Manufacturer | JLCPCB / PCBWay / Seeed / MacroFab | Like-for-like RFQs |

## Resolved
- **Project name:** Talos (2026-11-15).
- **Software license:** Apache-2.0 (ADR 0006).
- **ESP-IDF timing:** Phase 5 (ADR 0005).

## Not to be treated as implementation truth (spec §55)
Proposed H2 GPIO assignments, the 20-contact connector, BQ25186 plus boost stage, the 3 W camera slot and 500/600 mA rail targets, battery-life and solar calculations, vendor preprogramming claims, and heatmap precision beyond what sensor geometry supports.
