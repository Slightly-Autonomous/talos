# Architecture decision records

Short records of why each choice was made. A decision is changed by adding a new ADR that supersedes the old one; old ADRs are never edited in place except to mark them superseded.

| ADR | Title | Status |
|---|---|---|
| [0001](0001-monorepo-and-build.md) | Monorepo, Bazel for host C++, native tools elsewhere | Accepted |
| [0002](0002-protobuf-contracts.md) | Protobuf as the wire/storage contract, managed with buf | Accepted |
| [0003](0003-core-types-and-codec-boundary.md) | Core owns plain C++ types; codec is structural only | Accepted |
| [0004](0004-local-authority-and-command-lifecycle.md) | Hub authority and command lifecycle semantics | Accepted |
| [0005](0005-defer-esp-idf.md) | Defer the ESP-IDF project to Phase 5 | Accepted |
| [0006](0006-license.md) | Apache-2.0 for software | Accepted |
| [0007](0007-rules-v0.md) | Rule engine v0 semantics | Accepted |
| [0008](0008-journal-ordering-and-time.md) | Journal ordering, deduplication and time | Accepted |

Open questions live in [open-decisions.md](open-decisions.md).
