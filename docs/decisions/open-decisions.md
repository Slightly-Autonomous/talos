# Open decisions and risk register

A living register (spec §55). Nothing here is decided until an ADR, a test, a benchmark or a hardware experiment settles it. **Hardware numbers in `contracts/manifests/` are placeholders until measured.**

| Pri | Topic | Open question / risk | How it gets decided |
|---|---|---|---|
| P0 | Sensor connector | Exact coplanar/card-edge series, 18–20 contacts, current, ESD, analog pin, mating durability | Connector samples + mechanical/electrical DFM |
| P0 | GPIO/pin map | Exact ESP32-H2 module and pin mux, boot/flash/USB, deep-sleep wake on four slots | Datasheet + board validator + bench tests |
| P0 | Mainboard power | USB/PV ORing, charger (BQ25186 is a candidate only), 1S pack/NTC, rails, per-slot protection | Schematic, lab stress and thermal tests |
| P0 | Security trust | Owner-controlled signing, provisioning, eFuse flow, recovery, unique identities | Threat model + ESP-IDF reference + HIL |
| P0 | Camera performance | Snapshot cartridge vs IP camera; night/IR; pre-trigger buffering; real power | Camera benchmark against use cases |
| P0 | OTA resources | Is 4 MB flash enough? Partition sizes; update over Thread | Real images + HIL |
| P0 | Actuation safety | Strong authorization, local fail-safe, no policy bypass paths | Security invariants + hardware interlocks |
| P1 | DB authority/sync | SQLite specifics; Supabase projection/auth shape | Phase 3 slice + failure simulation |
| P1 | Client/API schema | gRPC vs JSON HTTP over the same protos; WebSocket/SSE semantics | Phase 3 proof of concept |
| P1 | Sensor specifics | Hall vs reed, accelerometer thresholds, PIR model | Prototype BOM, measured false alarms and power |
| P1 | Power budget | Real sleepy-node average, charger quiescent, pack life, panel size | Profiler measurements + weather scenarios |
| P1 | Four-slot modules | Per-slot ID EEPROM, duplicate I²C, multi-wake modules, UART conflicts | Electrical prototype + manifest tests |
| P1 | Dedup window | Is 1024 sequences per (device, boot) enough given device-side queue depth? (ADR 0008) | Firmware queue design, Phase 5/6 |
| P1 | NVR | Frigate vs custom adapter, RTSP/ONVIF profiles | Integration proof of concept |
| P1 | Hub hardware | Pi model vs mini-PC; MQTT placement; RCP firmware | Measured load |
| P1 | Twin calibration | All PIR/camera/impact/latency parameters in `simulation/scenarios/sites` are placeholders (ADR 0009) | Bench and field measurements, Phase 8 |
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
