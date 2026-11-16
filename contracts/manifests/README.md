# Manifests

YAML instances of the messages in `contracts/proto/talos/v1/manifest.proto` and
`property.proto`. They are loaded with `codec/yaml_loader`, which converts YAML
to protobuf JSON, so the `.proto` files are the only schema.

**Every number here is provisional.** None of the pin capabilities, rail limits,
currents or power budgets has been measured. They are placeholders so the
validator and the twin have something realistic-shaped to work with. Replace
them with bench measurements before relying on them (spec §38, §55).
