# ADR 0005: Defer the ESP-IDF project to Phase 5

**Status:** Accepted (2026-11-15).

The spec places an ESP-IDF skeleton in Phase 0. Nothing would exercise it before Phase 5 (virtual firmware), and the toolchain is a heavy install for contributors and CI. It is deferred.

Until then, core code stays embedded-friendly:
- no exceptions
- no protobuf in core
- small value types
- no hidden clocks or RNG

Phase 5 adds `firmware/portable` (host and target) and `firmware/esp_idf` (native CMake, CI build job). That phase is where portability is first proven on the real toolchain.
