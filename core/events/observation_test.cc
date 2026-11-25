#include "core/events/observation.h"

#include <gtest/gtest.h>

namespace talos {
namespace {

ObservationEnvelope valid() {
  ObservationEnvelope e;
  e.property = PropertyId("house-01");
  e.source = {DeviceId("node-07"), SlotIndex{0}, CapabilityId("motion.detect")};
  e.event_type = "motion.detected";
  e.event_id = EventId("node-07:1:5");
  e.position = {1, 5};
  e.payload = MotionState{true};
  return e;
}

TEST(ValidateEnvelope, AcceptsWellFormed) { EXPECT_TRUE(validate_envelope(valid())); }

TEST(ValidateEnvelope, RejectsMissingFields) {
  auto e = valid();
  e.event_id = EventId("");
  EXPECT_EQ(validate_envelope(e).error().code, "invalid_envelope");
  e = valid();
  e.payload = std::monostate{};
  EXPECT_FALSE(validate_envelope(e));
  e = valid();
  e.source.slot = SlotIndex{7};
  EXPECT_FALSE(validate_envelope(e));
}

TEST(ValidateEnvelope, RejectsUnknownSchemaVersion) {
  auto e = valid();
  e.schema_version = 2;
  EXPECT_EQ(validate_envelope(e).error().code, "unsupported_schema");
}

// ADR 0020: media ids reach a recorder URL, so only a plain id is accepted.
TEST(ValidateEnvelope, MediaReferencesAreBounded) {
  auto e = valid();
  e.source = {DeviceId("cam_patio_video"), std::nullopt, CapabilityId("camera.detection")};
  e.event_type = "camera.object_detected";
  e.event_id = EventId("cam_patio_video:frigate:1");
  e.payload =
      ObjectDetected{"person", 900, MediaRef{"frigate", "1700000000.25-x9k2", 1, 0, true, true}};
  EXPECT_TRUE(validate_envelope(e));
  for (std::string bad : {std::string("../config"), std::string("a/b"), std::string(".."),
                          std::string("x?y"), std::string(""), std::string(129, 'a')}) {
    std::get<ObjectDetected>(e.payload).media->media_id = bad;
    EXPECT_FALSE(validate_envelope(e)) << bad;
  }
  std::get<ObjectDetected>(e.payload).media = MediaRef{"Frigate", "ok", 0, 0, false, false};
  EXPECT_FALSE(validate_envelope(e));
  e.payload = ClipRecorded{"cmd:r1:e1", MediaRef{"frigate", "e1", 1, 2, true, false}};
  EXPECT_TRUE(validate_envelope(e));
  e.payload = ClipRecorded{"cmd 1", MediaRef{"frigate", "e1", 1, 2, true, false}};
  EXPECT_FALSE(validate_envelope(e));
}

}  // namespace
}  // namespace talos
