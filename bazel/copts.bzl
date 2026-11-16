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
# GCC emits false-positive -Wmaybe-uninitialized for std::variant at -O1+
# (seen under --config=asan); clang does not know the flag.
TALOS_PROTO_COPTS = [
    "-Wall",
    "-Wextra",
    "-Werror",
] + select({
    "@rules_cc//cc/compiler:gcc": ["-Wno-maybe-uninitialized"],
    "//conditions:default": [],
})
