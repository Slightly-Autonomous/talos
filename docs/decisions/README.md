# Architecture decision records

Short records of why each choice was made. A decision is changed by adding a new ADR that supersedes the old one; old ADRs are never edited in place except to mark them superseded.

| ADR | Title | Status |
|---|---|---|
| [0001](0001-monorepo-and-build.md) | Monorepo, Bazel for host C++, native tools elsewhere | Accepted |
| [0002](0002-protobuf-contracts.md) | Protobuf as the wire/storage contract, managed with buf | Accepted |
| [0003](0003-core-types-and-codec-boundary.md) | Core owns plain C++ types; codec is structural only | Accepted |
| [0004](0004-local-authority-and-command-lifecycle.md) | Hub authority and command lifecycle semantics | Accepted |
| [0005](0005-defer-esp-idf.md) | Defer the ESP-IDF project to Phase 5 | Superseded by 0013 |
| [0006](0006-license.md) | Apache-2.0 for software | Accepted |
| [0007](0007-rules-v0.md) | Rule engine v0 semantics | Accepted |
| [0008](0008-journal-ordering-and-time.md) | Journal ordering, deduplication and time | Accepted |
| [0009](0009-digital-twin-l1.md) | L1 digital twin and scenario runner | Accepted |
| [0010](0010-hub-server-and-deployment.md) | Hub server, storage, API and deployment switches | Accepted |
| [0011](0011-website-v1.md) | Website v1: live, replay, engineering, simulation | Accepted |
| [0012](0012-versioned-configuration-and-editors.md) | Versioned configuration, property and rule editors | Accepted |
| [0013](0013-portable-firmware-and-virtual-board.md) | Portable firmware, virtual board and ESP-IDF skeleton | Accepted |
| [0014](0014-device-network-ota-observability.md) | Device network (CoAP), signed OTA delivery, observability | Accepted |
| [0015](0015-integrations-and-remote-access.md) | Integrations, AI connector and remote access | Accepted |
| [0016](0016-hardware-bring-up-and-hil.md) | Hardware bring-up path and HIL testing | Accepted |
| [0017](0017-custom-hardware-workstream.md) | Custom hardware workstream (H0–H3); hardware ADRs in `hardware/architecture/decisions/` | Accepted |

Open questions live in [open-decisions.md](open-decisions.md).
