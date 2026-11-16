"""Compiler options applied to Talos-owned C++ targets only (never third-party)."""

TALOS_COPTS = [
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Wshadow",
    "-Wconversion",
    "-Wno-sign-conversion",
    "-Werror",
]

# For targets that include protobuf/abseil headers, which are not clean under
# -Wpedantic/-Wconversion/-Wshadow. Core libraries keep TALOS_COPTS.
TALOS_PROTO_COPTS = [
    "-Wall",
    "-Wextra",
    "-Werror",
]
