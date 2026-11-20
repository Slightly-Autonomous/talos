#include "core/domain/property.h"

#include <gtest/gtest.h>

namespace talos {
namespace {

Property sample() {
  Property p;
  p.id = PropertyId("house-01");
  p.zones = {{ZoneId("backyard"), "Backyard", ZoneKind::kOutdoor},
             {ZoneId("hallway"), "Hallway", ZoneKind::kIndoor}};
  Device node;
  node.id = DeviceId("node-07");
  node.property = p.id;
  node.zone = ZoneId("backyard");
  node.capabilities = {CapabilityId("contact.open_state")};
  p.devices.push_back(node);
  Asset door;
  door.id = AssetId("front_door");
  door.type = "portal.door";
  door.zone = ZoneId("hallway");
  door.attached.push_back({DeviceId("node-07"), SlotIndex{0}, CapabilityId("contact.open_state")});
  p.assets.push_back(door);
  return p;
}

TEST(ValidateProperty, AcceptsConsistentProperty) { EXPECT_TRUE(validate_property(sample())); }

TEST(ValidateProperty, RejectsDeviceFromAnotherProperty) {
  Property p = sample();
  p.devices[0].property = PropertyId("house-02");
  Status s = validate_property(p);
  ASSERT_FALSE(s);
  EXPECT_EQ(s.error().code, "cross_property_reference");
}

TEST(ValidateProperty, RejectsAssetAttachingForeignDevice) {
  Property p = sample();
  p.assets[0].attached.push_back({DeviceId("neighbors-node"), std::nullopt, CapabilityId("x")});
  Status s = validate_property(p);
  ASSERT_FALSE(s);
  EXPECT_EQ(s.error().code, "cross_property_reference");
}

// Red team #15: an empty list declares nothing (fail closed), not everything.
TEST(ValidateProperty, EmptyCapabilityListDeclaresNone) {
  Device d;
  d.id = DeviceId("node-07");
  EXPECT_FALSE(d.declares(CapabilityId("access.lock")));
  EXPECT_TRUE(d.declares(CapabilityId("device.health")));  // implied for every device
  Property p = sample();
  p.devices[0].capabilities.clear();
  Status s = validate_property(p);
  ASSERT_FALSE(s);
  EXPECT_EQ(s.error().code, "undeclared_capability");
}

TEST(ValidateProperty, RejectsAssetAttachingUndeclaredCapability) {
  Property p = sample();
  p.devices[0].capabilities = {CapabilityId("motion.detect")};
  Status s = validate_property(p);
  ASSERT_FALSE(s);
  EXPECT_EQ(s.error().code, "undeclared_capability");
  p.devices[0].capabilities.push_back(CapabilityId("contact.open_state"));
  EXPECT_TRUE(validate_property(p));
}

TEST(ValidateProperty, RejectsDuplicateAndDanglingIds) {
  Property dup = sample();
  dup.zones.push_back({ZoneId("backyard"), "again", ZoneKind::kOutdoor});
  EXPECT_EQ(validate_property(dup).error().code, "duplicate_id");

  Property dangling = sample();
  dangling.assets[0].zone = ZoneId("attic");
  EXPECT_EQ(validate_property(dangling).error().code, "unknown_zone");
}

TEST(Property, ZoneOfPrefersAssetZoneThenDevicePlacement) {
  Property p = sample();
  EXPECT_EQ(p.zone_of({DeviceId("node-07"), SlotIndex{0}, CapabilityId("contact.open_state")}),
            ZoneId("hallway"));
  EXPECT_EQ(p.zone_of({DeviceId("node-07"), SlotIndex{1}, CapabilityId("motion.detect")}),
            ZoneId("backyard"));
  EXPECT_EQ(p.zone_of({DeviceId("ghost"), std::nullopt, CapabilityId("x")}), std::nullopt);
}

}  // namespace
}  // namespace talos
