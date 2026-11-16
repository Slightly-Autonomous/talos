# firmware

- `portable/`: device-side C++ shared between host simulation and the MCU (Phase 5). Must stay free of ESP-IDF, heap churn on hot paths, and exceptions.
- `esp_idf/`: ESP32-H2 CMake project (Phase 5, ADR 0005). Not created yet.

Nothing in core assumes a specific MCU, pin map or connector.
