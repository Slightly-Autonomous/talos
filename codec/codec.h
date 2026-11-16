// Structural mapping between wire/storage protobufs and core value types.
//
// The codec does shape conversion and range checks only. Ownership,
// cross-property and semantic validation belong to the domain/application
// layers (core/domain, core/ingest, core/policy). Unknown protobuf fields are
// preserved only when handling protobuf messages directly; a
// proto -> core -> proto round trip carries known fields only (ADR 0003).
#pragma once

#include "core/base/result.h"
#include "core/domain/property.h"
#include "core/events/observation.h"
#include "core/execution/command.h"
#include "core/journal/journal.h"
#include "core/manifest/manifest.h"
#include "core/rules/rules.h"
#include "talos/v1/command.pb.h"
#include "talos/v1/journal.pb.h"
#include "talos/v1/manifest.pb.h"
#include "talos/v1/observation.pb.h"
#include "talos/v1/property.pb.h"
#include "talos/v1/rule.pb.h"

namespace talos::codec {

Result<ObservationEnvelope> from_proto(const talos::v1::ObservationEnvelope& p);
talos::v1::ObservationEnvelope to_proto(const ObservationEnvelope& e);

Result<Command> from_proto(const talos::v1::Command& p);
talos::v1::Command to_proto(const Command& c);

Result<CommandTransition> from_proto(const talos::v1::CommandTransition& p);
talos::v1::CommandTransition to_proto(const CommandTransition& t);

Result<JournalEntry> from_proto(const talos::v1::JournalEntry& p);
talos::v1::JournalEntry to_proto(const JournalEntry& e);

Result<CapabilityRef> from_proto(const talos::v1::CapabilityRef& p);
ObservationPayload from_proto(const talos::v1::ObservationPayload& p);
Result<CommandArgs> from_proto(const talos::v1::CommandArgs& p);
Result<ArmMode> from_proto(talos::v1::ArmMode m);
Result<Rule> from_proto(const talos::v1::Rule& p);

Result<Property> from_proto(const talos::v1::Property& p);
Result<BoardManifest> from_proto(const talos::v1::BoardManifest& p);
Result<ModuleManifest> from_proto(const talos::v1::ModuleManifest& p);

}  // namespace talos::codec
