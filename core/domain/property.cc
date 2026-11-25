#include "core/domain/property.h"

#include <set>

namespace talos {

const Zone* Property::find_zone(const ZoneId& zid) const {
  for (const auto& z : zones) {
    if (z.id == zid) return &z;
  }
  return nullptr;
}

const Device* Property::find_device(const DeviceId& did) const {
  for (const auto& d : devices) {
    if (d.id == did) return &d;
  }
  return nullptr;
}

bool Device::declares(const CapabilityId& capability) const {
  if (capability.str() == "device.health") return true;
  for (const auto& c : capabilities) {
    if (c == capability) return true;
  }
  return false;
}

std::optional<ZoneId> Property::zone_of(const CapabilityRef& ref) const {
  for (const auto& asset : assets) {
    for (const auto& att : asset.attached) {
      if (att.device == ref.device && att.capability == ref.capability &&
          (!att.slot || !ref.slot || att.slot == ref.slot) && asset.zone) {
        return asset.zone;
      }
    }
  }
  if (const Device* d = find_device(ref.device)) return d->zone;
  return std::nullopt;
}

namespace {

Error err(std::string code, std::string message) { return {std::move(code), std::move(message)}; }

}  // namespace

Status validate_property(const Property& p) {
  if (p.id.empty()) return err("invalid_property", "property id is empty");

  std::set<ZoneId> zone_ids;
  for (const auto& z : p.zones) {
    if (z.id.empty()) return err("invalid_zone", "zone id is empty");
    if (!zone_ids.insert(z.id).second) return err("duplicate_id", "duplicate zone " + z.id.str());
  }

  std::set<DeviceId> device_ids;
  for (const auto& d : p.devices) {
    if (d.id.empty()) return err("invalid_device", "device id is empty");
    if (!device_ids.insert(d.id).second) {
      return err("duplicate_id", "duplicate device " + d.id.str());
    }
    if (d.property != p.id) {
      return err("cross_property_reference",
                 "device " + d.id.str() + " belongs to property " + d.property.str());
    }
    if (d.zone && !zone_ids.contains(*d.zone)) {
      return err("unknown_zone", "device " + d.id.str() + " placed in unknown zone");
    }
  }
  // A host is one level deep: a camera's Frigate view names its Vision Node,
  // which has no host of its own (ADR 0020).
  for (const auto& d : p.devices) {
    if (!d.host) continue;
    const Device* host = nullptr;
    for (const auto& h : p.devices) {
      if (h.id == *d.host) host = &h;
    }
    if (!host || host->id == d.id || host->host) {
      return err("invalid_host", "device " + d.id.str() + ": host " + d.host->str() +
                                     " must be another configured device with no host");
    }
  }

  std::set<AssetId> asset_ids;
  for (const auto& a : p.assets) {
    if (a.id.empty()) return err("invalid_asset", "asset id is empty");
    if (!asset_ids.insert(a.id).second) return err("duplicate_id", "duplicate asset " + a.id.str());
    if (a.zone && !zone_ids.contains(*a.zone)) {
      return err("unknown_zone", "asset " + a.id.str() + " in unknown zone");
    }
    for (const auto& att : a.attached) {
      if (!device_ids.contains(att.device)) {
        return err("cross_property_reference", "asset " + a.id.str() + " attaches device " +
                                                   att.device.str() +
                                                   " that is not part of this property");
      }
      if (att.slot && *att.slot >= kMaxSlots) {
        return err("invalid_slot", "asset " + a.id.str() + " references slot out of range");
      }
      if (const Device* d = p.find_device(att.device); d && !d->declares(att.capability)) {
        return err("undeclared_capability", "asset " + a.id.str() + " attaches " +
                                                att.capability.str() + ", which " +
                                                att.device.str() + " does not declare");
      }
    }
  }
  return Status::Ok();
}

}  // namespace talos
