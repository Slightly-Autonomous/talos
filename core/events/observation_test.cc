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

}  // namespace
}  // namespace talos
