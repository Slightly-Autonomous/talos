#include "codec/codec.h"

#include <gtest/gtest.h>

#include "core/testing/fixtures.h"

namespace talos::codec {
namespace {

Command sample_command() {
  Command c;
  c.id = CommandId("cmd-1");
  c.idempotency_key = "k";
  c.principal = PrincipalId("user-33");
  c.property = PropertyId("house-01");
  c.target = {DeviceId("patio_speaker"), std::nullopt, CapabilityId("audio.speak")};
  c.operation = "audio.speak";
  c.args = AudioSpeak{"hello", 65};
  c.created_at = Timestamp{1};
  c.expires_at = Timestamp{5001};
  c.verification = VerificationMode::kObservedState;
  c.verification_window = Millis{3000};
  c.expect = ExpectedObservation{c.target, AudioPlayback{true}};
  c.causation = EventId("node-07:1:1");
  return c;
}

TEST(Codec, ObservationRoundTripsKnownFields) {
  auto e = testing::motion("node-07", 3, 42);
  e.observed_at = Timestamp{123};
  e.module = ModuleId("pir_ekmb_v1");
  e.causation = EventId("x");
  auto back = from_proto(to_proto(e));
  ASSERT_TRUE(back.ok());
  EXPECT_EQ(back.value(), e);
}

TEST(Codec, AllPayloadKindsRoundTrip) {
  std::vector<ObservationPayload> payloads = {ContactState{true},
                                              MotionState{true},
                                              ImpactDetected{-840, 12},
                                              TemperatureHumidity{21500, 455},
                                              LockState{true},
                                              AudioPlayback{true},
                                              ObjectDetected{"person", 870},
                                              Heartbeat{3700}};
  for (const auto& p : payloads) {
    auto e = testing::motion("node-07", 1, 1);
    e.payload = p;
    EXPECT_EQ(from_proto(to_proto(e)).value().payload, p);
  }
}

TEST(Codec, CommandAndTransitionRoundTrip) {
  Command c = sample_command();
  EXPECT_EQ(from_proto(to_proto(c)).value(), c);

  CommandTransition t;
  t.command = c.id;
  t.to = CommandStatus::kRequested;
  t.at = Timestamp{7};
  t.reason = "requested";
  t.created = c;
  t.late = true;
  t.evidence = EventId("ev");
  EXPECT_EQ(from_proto(to_proto(t)).value(), t);
}

TEST(Codec, JournalEntryRoundTrip) {
  JournalEntry obs{4, Timestamp{99}, true,
                   ObservationRecorded{testing::motion("n", 1, 2), SourceOrder::kGap}};
  EXPECT_EQ(from_proto(to_proto(obs)).value(), obs);
  CommandTransition t;
  t.command = CommandId("c");
  t.to = CommandStatus::kVerified;
  JournalEntry cmd{5, Timestamp{100}, false, t};
  EXPECT_EQ(from_proto(to_proto(cmd)).value(), cmd);
  JournalEntry arm{6, Timestamp{101}, false,
                   ArmModeChanged{ArmMode::kArmedAway, PrincipalId("user-33")}};
  EXPECT_EQ(from_proto(to_proto(arm)).value(), arm);
  JournalEntry rev{7, Timestamp{102}, false,
                   ConfigRevisionApplied{3, "deadbeef", PrincipalId("owner")}};
  EXPECT_EQ(from_proto(to_proto(rev)).value(), rev);
}

TEST(Codec, RejectsOutOfRangeAndInvalidEnums) {
  auto p = to_proto(testing::motion("node-07", 1, 1));
  p.mutable_source()->set_slot(300);
  EXPECT_EQ(from_proto(p).error().code, "out_of_range");

  auto cp = to_proto(sample_command());
  cp.mutable_args()->mutable_audio_speak()->set_volume_percent(1000);
  EXPECT_EQ(from_proto(cp).error().code, "out_of_range");

  cp = to_proto(sample_command());
  cp.set_verification(talos::v1::VERIFICATION_MODE_UNSPECIFIED);
  EXPECT_EQ(from_proto(cp).error().code, "invalid_enum");

  talos::v1::JournalEntry empty;
  EXPECT_EQ(from_proto(empty).error().code, "invalid_journal_entry");
}

TEST(Codec, RuleFromProto) {
  talos::v1::Rule p;
  p.set_id("r1");
  p.set_revision(2);
  p.set_property_id("house-01");
  p.set_principal_id("rule:r1");
  p.set_enabled(true);
  p.set_trigger_event_type("motion.detected");
  p.set_trigger_zone_id("backyard");
  p.add_armed_modes(talos::v1::ARM_MODE_ARMED_AWAY);
  p.mutable_schedule()->set_start_minute(1320);
  p.mutable_schedule()->set_end_minute(360);
  auto* a = p.add_actions();
  a->set_operation("audio.speak");
  a->mutable_target()->set_device_id("patio_speaker");
  a->mutable_target()->set_capability_id("audio.speak");
  a->mutable_args()->mutable_audio_speak()->set_text("hi");
  a->set_ttl_ms(4000);
  auto r = from_proto(p);
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_EQ(r.value().trigger.zone, ZoneId("backyard"));
  EXPECT_EQ(r.value().conditions.armed_modes.at(0), ArmMode::kArmedAway);
  EXPECT_EQ(r.value().actions.at(0).ttl, Millis{4000});
  EXPECT_EQ(r.value().actions.at(0).verification, VerificationMode::kObservedState);

  p.mutable_schedule()->set_start_minute(5000);
  EXPECT_EQ(from_proto(p).error().code, "out_of_range");
}

TEST(Codec, CodecDoesNotJudgeOwnership) {
  // A foreign property id is structurally fine; ingest/policy reject it.
  auto p = to_proto(testing::motion("node-07", 1, 1));
  p.set_property_id("someone-elses-house");
  EXPECT_TRUE(from_proto(p).ok());
}

TEST(Proto, UnknownFieldsSurviveProtoToProtoHandling) {
  // Simulates a newer sender adding field 99. Unknown-field preservation is a
  // property of protobuf-to-protobuf handling only (ADR 0003).
  auto p = to_proto(testing::motion("node-07", 1, 1));
  std::string bytes = p.SerializeAsString();
  // Field 99, wire type 2: key = (99 << 3) | 2 = 794, varint-encoded as 0x9A 0x06.
  bytes.push_back(static_cast<char>(0x9A));
  bytes.push_back(static_cast<char>(0x06));
  bytes.push_back(3);  // length
  bytes += "new";
  talos::v1::ObservationEnvelope parsed;
  ASSERT_TRUE(parsed.ParseFromString(bytes));
  EXPECT_EQ(parsed.SerializeAsString(), bytes);
  // Older readers still decode the known fields.
  EXPECT_EQ(from_proto(parsed).value(), testing::motion("node-07", 1, 1));
}

}  // namespace
}  // namespace talos::codec
