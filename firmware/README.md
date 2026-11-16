# firmware

- `portable/`: device firmware core (NodeRuntime, module drivers, OTA state machine, bounded queue). It is shared by the host tests, the twin and the ESP32-H2 build. No heap in hot paths, no exceptions, no RTTI.
- `sim/`: virtual board (register-level I2C mux/EEPROM/LIS2DW12/SHT40 models, GPIO, power, store with power-cut injection, A/B flash, lossy link), plus a bridge to the real hub engine. Tests: `bazel test //firmware/...`.
- `esp_idf/`: ESP32-H2 project compiling the same portable sources (`idf.py set-target esp32h2 build`). It's a skeleton: clock, GPIO, I2C and NVS adapters are real. Battery sensing, OTA flash binding and the Thread link are not. **Pin numbers are provisional placeholders.**

See ADR 0013.
