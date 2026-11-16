// Board and module manifests plus the slot-configuration validator (spec §31-32).
//
// Every concrete number in a manifest (pins, rails, currents, power budgets)
// is PROVISIONAL until measured on hardware; manifests carry a `provisional`
// flag and the validator reports against whatever the data says. Board
// manifests are build/release artifacts: nothing at runtime can rewrite them.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/domain/capability.h"
#include "core/domain/ids.h"

namespace talos {

enum class PowerSource : std::uint8_t { kBatteryOnly, kUsb, kSolarBattery };

std::string_view to_string(PowerSource s);

// What a power source can sustain on average and deliver at peak, in mW.
struct PowerSourceBudget {
  PowerSource source = PowerSource::kBatteryOnly;
  std::uint32_t sustained_mw = 0;
  std::uint32_t peak_mw = 0;
};

struct RailSpec {
  std::string name;  // e.g. "v3v3_sw"
  std::uint32_t voltage_mv = 0;
  std::uint32_t max_ma = 0;           // total across all slots
  std::uint32_t per_slot_max_ma = 0;  // per-slot switch/limiter limit
};

struct SlotSpec {
  SlotIndex index = 0;
  bool wake = false;                       // dedicated wake-capable line
  std::optional<std::uint8_t> i2c_branch;  // mux branch; slots sharing a branch share addresses
  bool spi = false;
  bool uart = false;
  bool analog = false;
  std::vector<std::string> rails;  // rails routed to this slot
};

struct BoardManifest {
  BoardId id;
  std::string revision;
  bool provisional = true;
  std::vector<SlotSpec> slots;
  std::vector<RailSpec> rails;
  std::vector<PowerSourceBudget> power_sources;
};

struct RailLoad {
  std::string rail;
  std::uint32_t active_ma = 0;
};

struct ModuleManifest {
  ModuleId id;
  std::string revision;
  bool provisional = true;
  bool needs_wake = false;
  std::vector<std::uint8_t> i2c_addresses;  // 7-bit
  bool needs_spi = false;
  bool needs_uart = false;
  bool needs_analog = false;
  std::vector<RailLoad> loads;
  std::uint32_t average_mw = 0;  // expected average draw in its configured duty cycle
  std::uint32_t peak_mw = 0;     // worst-case instantaneous draw
  std::vector<CapabilityId> capabilities;
};

using ModuleCatalog = std::map<ModuleId, ModuleManifest>;

struct SlotAssignment {
  SlotIndex slot = 0;
  ModuleId module;
};

enum class ManifestIssueCode : std::uint8_t {
  kUnknownSlot,
  kDuplicateSlot,
  kUnknownModule,
  kMissingWake,
  kMissingBus,
  kI2cAddressConflict,
  kRailNotRouted,
  kRailSlotLimit,
  kRailTotalLimit,
  kPowerSourceUnsupported,
  kSustainedPowerExceeded,
  kPeakPowerExceeded,
};

std::string_view to_string(ManifestIssueCode c);

struct ManifestIssue {
  ManifestIssueCode code;
  std::optional<SlotIndex> slot;
  std::string message;
};

struct ValidationResult {
  std::vector<ManifestIssue> issues;
  bool ok() const { return issues.empty(); }
  bool has(ManifestIssueCode code) const;
};

// Checks that `assignments` can run on `board` when powered by `active_source`.
// Pure; reports every problem found rather than stopping at the first.
ValidationResult validate_configuration(const BoardManifest& board, const ModuleCatalog& catalog,
                                        const std::vector<SlotAssignment>& assignments,
                                        PowerSource active_source);

}  // namespace talos
