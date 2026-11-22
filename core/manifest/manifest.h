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
  std::uint32_t sustained_uw = 0;  // used instead of sustained_mw when non-zero
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
  // Built-in module (ADR 0019): soldered on, always populated with exactly this
  // module. Connector, bus, per-slot rail and slot-limit checks do not apply;
  // its I2C devices sit on the root bus.
  std::optional<ModuleId> built_in_module;
};

// The board's battery (ADR 0019); thresholds under load for primary cells.
struct BatterySpec {
  std::string chemistry;  // "li-fes2", "li-ion"
  std::string form;       // "AA", "18650"
  std::uint32_t cells_in_series = 0;
  bool rechargeable = false;  // false = primary: never charged
  std::uint32_t fresh_mv = 0;
  std::uint32_t low_mv = 0;
  std::uint32_t critical_mv = 0;
};

// On-board wiring that firmware drives (documentation, PROVISIONAL).
struct ExpanderPinSpec {
  std::string pin;        // "P00".."P17"
  std::string net;        // empty for an unconnected pin
  std::string direction;  // "input" | "output" | "unused"
  bool active_low = false;
};

struct ExpanderSpec {
  std::string name;  // e.g. "EXP_C"
  std::string part;
  std::uint8_t i2c_address = 0;
  std::string interrupt_net;
  std::vector<ExpanderPinSpec> pins;
};

struct WakeLineSpec {
  std::string net;
  std::string mcu_pin;
  bool active_high = true;
};

struct IndicatorSpec {
  std::string name;
  std::string net;
  std::string driver;
  bool active_low = false;
  std::string color;
};

// A module only some of the board's slots may host (B-03), e.g. a T/RH card
// kept away from the charger's heat. PROVISIONAL data from the board manifest.
struct ModuleSlotLimit {
  ModuleId module;
  std::vector<SlotIndex> allowed_slots;
  std::string reason;
};

struct BoardManifest {
  BoardId id;
  std::string revision;
  bool provisional = true;
  std::vector<SlotSpec> slots;
  std::vector<RailSpec> rails;
  std::vector<PowerSourceBudget> power_sources;
  std::string connector_standard;                    // empty = not declared
  std::uint32_t uart_channels = 0;                   // concurrent card UARTs; 0 = not declared
  std::vector<std::uint8_t> reserved_i2c_addresses;  // root-bus addresses cards must avoid
  std::string board_family;
  std::string transport;
  std::vector<ExpanderSpec> expanders;
  std::vector<WakeLineSpec> wake_lines;
  std::vector<IndicatorSpec> indicators;
  std::vector<CapabilityId> board_capabilities;  // provided by the board, not a slot
  std::vector<ModuleSlotLimit> module_slot_limits;
  std::optional<BatterySpec> battery;
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
  std::uint32_t average_uw = 0;  // used instead of average_mw when non-zero
  std::vector<CapabilityId> capabilities;
  std::string connector_standard;  // empty = not declared
  bool test_fixture = false;  // bench fixture: exempt from the concurrent UART limit (time-shared)
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
  kConnectorMismatch,
  kI2cRootConflict,
  kUartOversubscribed,
  kSlotNotAllowed,   // the board limits this module to other slots (ModuleSlotLimit)
  kBuiltInMismatch,  // a built-in slot assigned a different module than the one soldered on
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
// Built-in slots (ADR 0019) are always populated: an assignment may name them
// (it must name the built-in module) or leave them out; either way they count.
ValidationResult validate_configuration(const BoardManifest& board, const ModuleCatalog& catalog,
                                        const std::vector<SlotAssignment>& assignments,
                                        PowerSource active_source);

}  // namespace talos
