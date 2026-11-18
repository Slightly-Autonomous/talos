# Open decisions and risk register

A living register (spec §55). Nothing here is decided until an ADR, a test, a benchmark or a hardware experiment settles it. **Hardware numbers in `contracts/manifests/` are placeholders until measured.** The detailed hardware register (HD-001..HD-019, with evidence and next actions) is [`hardware/requirements/design-risks.md`](../../hardware/requirements/design-risks.md).

| Pri | Topic | Open question / risk | How it gets decided |
|---|---|---|---|
| P0 | Sensor connector | Series chosen 2026-11-17: Samtec HSEC8-110-01-S-D-EM2, 20 contacts, 30 µin gold (hardware HW-0007). Still open: land pattern from Samtec's drawing, mating durability, enclosure clamping against fretting | Samtec drawing + samples + mechanical DFM |
| P0 | GPIO/pin map | Exact ESP32-H2 module and pin mux, boot/flash/USB, deep-sleep wake on four slots | Datasheet + board validator + bench tests |
| P0 | Mainboard power | USB/PV ORing, charger (BQ25186 is a candidate only), 1S pack/NTC, rails, per-slot protection | Schematic, lab stress and thermal tests |
| P0 | Security trust | Owner-controlled signing, provisioning, eFuse flow, recovery, unique identities | Threat model + ESP-IDF reference + HIL |
| P0 | Camera performance | Decided: separate Vision Node, no camera cartridge (HW-0001). Open: compute/codec, night/IR, pre-trigger buffering, real power | H5 camera benchmark |
| P0 | OTA resources | Is 4 MB flash enough? Partition sizes; update over Thread | Real images + HIL |
| P0 | Actuation safety | Strong authorization, local fail-safe, no policy bypass paths | Security invariants + hardware interlocks |
| P1 | Postgres driver | libpq via Bazel for `STORAGE_DRIVER_POSTGRES` (cloud-hosted hub on Supabase/Postgres) | Implement behind `IJournal` + conformance suite |
| P1 | Remote access | Relay role, Supabase JWT auth, per-principal read scopes | Remote-access phase (ADR 0010) |

| P1 | Sensor specifics | Hall vs reed, accelerometer thresholds, PIR model | Prototype BOM, measured false alarms and power |
| P1 | Power budget | Real sleepy-node average, charger quiescent, pack life, panel size | Profiler measurements + weather scenarios |
| P1 | Four-slot modules | Per-slot ID EEPROM, duplicate I²C, multi-wake modules, UART conflicts | Electrical prototype + manifest tests |
| P0 | Device provisioning | Claim/commissioning flow that generates per-device CoAP keys (today a YAML file, ADR 0014) | Spec §37 design + HIL |
| P1 | Thread validation | OTNS/OpenThread POSIX harness for partitions, parent switching, sleepy polling | Linux CI job with OTNS |
| P1 | Dedup window | Is 1024 sequences per (device, boot) enough given device-side queue depth? (ADR 0008) | Firmware queue design, Phase 5/6 |
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
