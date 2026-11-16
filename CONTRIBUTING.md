# Contributing

1. Read `docs/architecture/overview.md` and the ADRs.
2. Write tests with the change. C++ tests use GoogleTest under Bazel; web tests use Vitest.
3. Run `bazel test //...` and `pnpm web:check` locally. CI runs the same checks, plus sanitizers, clang-format and `buf breaking`.
4. Architecture or contract changes need an ADR, or an update that supersedes an existing one.
5. Protobuf: never renumber or reuse fields. Reserve removed ones.
6. Hardware values go in manifests marked `provisional: true` until measured.
