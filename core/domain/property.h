// Property model: the spatial and asset structure of one installation.
//
// Phase 1 keeps geometry out (polygons, levels, coverage arrive with the twin
// in Phase 2); this is the relational skeleton that rules and authorization
// need: zones, assets, devices and where capabilities are placed.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/base/result.h"
#include "core/domain/capability.h"
#include "core/domain/ids.h"

namespace talos {

enum class ZoneKind { kIndoor, kOutdoor };

enum class ArmMode : std::uint8_t { kDisarmed, kArmedHome, kArmedAway };

struct Zone {
  ZoneId id;
  std::string name;
  ZoneKind kind = ZoneKind::kIndoor;
};

// A physical thing (door, window, gate, speaker location), distinct from the
// devices that observe or control it.
struct Asset {
  AssetId id;
  std::string type;  // e.g. "portal.door"
  std::optional<ZoneId> zone;
  std::vector<CapabilityRef> attached;
};

inline constexpr std::size_t kMaxSlots = 4;

struct Device {
  DeviceId id;
  PropertyId property;
  std::optional<BoardId> board;
  std::optional<std::string> board_revision;  // e.g. "proto-a"; OTA compatibility
  // Module type installed in each slot, if any. Devices without slots (an IP
  // speaker, a lock) leave all entries empty.
  std::array<std::optional<ModuleId>, kMaxSlots> slots{};
  std::optional<ZoneId> zone;  // where the device is placed
  // Capabilities this device may report and be commanded on. Fail closed: an
  // empty list declares none. device.health is implied for every device.
  std::vector<CapabilityId> capabilities;
  // The device this one belongs to (a camera's Frigate view names its Vision
  // Node, ADR 0020). Shown as one unit; must name another configured device.
  std::optional<DeviceId> host;

  bool declares(const CapabilityId& capability) const;
};

struct Property {
  PropertyId id;
  std::string timezone;  // IANA name; time-zone math happens outside core
  std::vector<Zone> zones;
  std::vector<Asset> assets;
  std::vector<Device> devices;

  const Zone* find_zone(const ZoneId& id) const;
  const Device* find_device(const DeviceId& id) const;
  // Zone of the capability: an attached asset's zone wins, else device placement.
  std::optional<ZoneId> zone_of(const CapabilityRef& ref) const;
};

// Referential and ownership integrity: unique IDs, zone references exist,
// every device belongs to this property, every asset attachment points at a
// device of this property. This is the domain-level cross-property check.
Status validate_property(const Property& property);

}  // namespace talos
