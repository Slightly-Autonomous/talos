# ADR 0003: Core owns plain C++ types; codec is structural only

**Status:** Accepted (2026-11-15).

## Context
Using generated protobuf classes directly in reducers and state machines would couple core logic to the full protobuf runtime, which portable firmware cannot carry. It also makes pure functions clumsy to write.

## Decision
- `core/*` defines its own small value types (`ObservationEnvelope`, `Command`, `CommandTransition`, manifests, and so on), with strong ID types and explicit units. Core never includes protobuf.
- `codec/` maps between protobuf and core. It does **shape conversion and range checks only**, such as a slot index fitting in `uint8` or an enum not being `UNSPECIFIED`.
- **Ownership and cross-property checks do not belong in the codec.** They live at domain and application boundaries:
  - `validate_property()`: devices and asset attachments must belong to the property.
  - `Ingestor`: observations must name this property and a known device.
  - `policy::evaluate()`: a principal needs a grant for the command's property.
- **Unknown-field preservation** is a protobuf property and holds only while a message stays protobuf (proto→proto, e.g. relaying or storing). A proto→core→proto round trip carries **known fields only**, and the tests assert exactly that.
- Core is exception-free and uses `talos::Result`/`Status`. The YAML loader catches yaml-cpp exceptions at its boundary.

## Consequences
There is some mapping code to maintain, and the round-trip tests guard it. Core stays reusable by Phase 5 portable firmware.
