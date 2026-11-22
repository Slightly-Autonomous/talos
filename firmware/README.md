# firmware

- `portable/`: device firmware core (NodeRuntime, module drivers, OTA state machine, bounded queue). It is shared by the host tests, the twin and the ESP32-H2 build. No heap in hot paths, no exceptions, no RTTI.
- `sim/`: virtual board (register-level I2C mux/EEPROM/LIS2DW12/SHT40 models, GPIO, power, store with power-cut injection, A/B flash, lossy link), plus a bridge to the real hub engine. Tests: `bazel test //firmware/...`.
- `esp_idf/`: ESP32-H2 project compiling the same portable sources (`idf.py set-target esp32h2 build`). It's a skeleton: clock, GPIO, I2C and NVS adapters are real. Battery sensing, OTA flash binding and the Thread link are not. **Pin numbers are provisional placeholders.**

Boards: TL-SN-002 (self-contained, ADR 0019; `esp_idf/main/board_tl_sn_002.h`) is the default ESP-IDF
target (Kconfig `TALOS_BOARD`); the archived TL-SN-001 builds with
`idf.py -B build_tl_sn_001 -D SDKCONFIG=build_tl_sn_001/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.tl_sn_001" set-target esp32h2 build`.
The virtual board models both (`sim::tl_sn_001_board()`, `sim::tl_sn_002_board()`; `bench_node --board=TL-SN-002`).

See ADR 0013 (and its 2026-11-21 (b) amendment for built-in modules).
