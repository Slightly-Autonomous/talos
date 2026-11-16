# ADR 0001: Monorepo, Bazel for host C++, native tools elsewhere

**Status:** Accepted (2026-11-15). Implements spec D14 and §44.

## Context
Talos spans C++ (hub core, simulation, portable firmware), TypeScript (web), protobuf contracts and later ESP-IDF firmware. Contracts and core logic are shared across all of them, so they belong in one repository.

## Decision
- One monorepo.
- **Bazel 8 (Bzlmod)** builds and tests all host C++ and generated C++ protobuf code. The version is pinned in `.bazelversion` and run through bazelisk. Dependencies come from the Bazel Central Registry: `rules_cc`, `protobuf`, `yaml-cpp`, `googletest`.
- C++20 everywhere. Talos-owned targets use `TALOS_COPTS` (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`). Targets that include protobuf or abseil headers use the narrower `TALOS_PROTO_COPTS`, because those headers are not clean under pedantic/conversion warnings.
- **pnpm** builds the web app, invoked from CI. The web app is not wrapped in Bazel.
- ESP-IDF will use its native CMake build, invoked from CI (ADR 0005). It will not be wrapped in Bazel.
- `--config=asan` and `--config=ubsan` run the C++ suite under sanitizers.

## Consequences
Builds are reproducible and incremental for the C++ core. Two build systems exist, which is accepted because §44 explicitly warns against forcing ESP-IDF and Node into Bazel early.
