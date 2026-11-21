#include "core/manifest/manifest.h"

#include <algorithm>
#include <set>

namespace talos {

std::string_view to_string(PowerSource s) {
  switch (s) {
    case PowerSource::kBatteryOnly:
      return "battery_only";
    case PowerSource::kUsb:
      return "usb";
    case PowerSource::kSolarBattery:
      return "solar_battery";
  }
  return "invalid";
}

std::string_view to_string(ManifestIssueCode c) {
  switch (c) {
    case ManifestIssueCode::kUnknownSlot:
      return "unknown_slot";
    case ManifestIssueCode::kDuplicateSlot:
      return "duplicate_slot";
    case ManifestIssueCode::kUnknownModule:
      return "unknown_module";
    case ManifestIssueCode::kMissingWake:
      return "missing_wake";
    case ManifestIssueCode::kMissingBus:
      return "missing_bus";
    case ManifestIssueCode::kI2cAddressConflict:
      return "i2c_address_conflict";
    case ManifestIssueCode::kRailNotRouted:
      return "rail_not_routed";
    case ManifestIssueCode::kRailSlotLimit:
      return "rail_slot_limit";
    case ManifestIssueCode::kRailTotalLimit:
      return "rail_total_limit";
    case ManifestIssueCode::kPowerSourceUnsupported:
      return "power_source_unsupported";
    case ManifestIssueCode::kSustainedPowerExceeded:
      return "sustained_power_exceeded";
    case ManifestIssueCode::kPeakPowerExceeded:
      return "peak_power_exceeded";
    case ManifestIssueCode::kConnectorMismatch:
      return "connector_mismatch";
    case ManifestIssueCode::kI2cRootConflict:
      return "i2c_root_conflict";
    case ManifestIssueCode::kUartOversubscribed:
      return "uart_oversubscribed";
    case ManifestIssueCode::kSlotNotAllowed:
      return "slot_not_allowed";
  }
  return "invalid";
}

bool ValidationResult::has(ManifestIssueCode code) const {
  for (const auto& i : issues) {
    if (i.code == code) return true;
  }
  return false;
}

namespace {

const SlotSpec* find_slot(const BoardManifest& b, SlotIndex idx) {
  for (const auto& s : b.slots) {
    if (s.index == idx) return &s;
  }
  return nullptr;
}

const RailSpec* find_rail(const BoardManifest& b, const std::string& name) {
  for (const auto& r : b.rails) {
    if (r.name == name) return &r;
  }
  return nullptr;
}

std::string slot_str(SlotIndex s) { return "slot " + std::to_string(s); }

}  // namespace

ValidationResult validate_configuration(const BoardManifest& board, const ModuleCatalog& catalog,
                                        const std::vector<SlotAssignment>& assignments,
                                        PowerSource active_source) {
  ValidationResult r;
  auto issue = [&](ManifestIssueCode code, std::optional<SlotIndex> slot, std::string msg) {
    r.issues.push_back({code, slot, std::move(msg)});
  };

  struct Placed {
    const SlotSpec* slot;
    const ModuleManifest* module;
  };
  std::vector<Placed> placed;
  std::set<SlotIndex> used;

  for (const auto& a : assignments) {
    const SlotSpec* slot = find_slot(board, a.slot);
    if (!slot) {
      issue(ManifestIssueCode::kUnknownSlot, a.slot, slot_str(a.slot) + " does not exist");
      continue;
    }
    if (!used.insert(a.slot).second) {
      issue(ManifestIssueCode::kDuplicateSlot, a.slot, slot_str(a.slot) + " assigned twice");
      continue;
    }
    auto mit = catalog.find(a.module);
    if (mit == catalog.end()) {
      issue(ManifestIssueCode::kUnknownModule, a.slot, "unknown module " + a.module.str());
      continue;
    }
    const ModuleManifest& m = mit->second;
    // A card built for another connector revision is never activated (HW-MF-007).
    if (!board.connector_standard.empty() && m.connector_standard != board.connector_standard) {
      issue(ManifestIssueCode::kConnectorMismatch, a.slot,
            m.id.str() + " is built for connector '" + m.connector_standard + "', board has '" +
                board.connector_standard + "'");
      continue;
    }
    placed.push_back({slot, &m});

    for (const auto& lim : board.module_slot_limits) {
      if (lim.module != m.id) continue;
      if (std::find(lim.allowed_slots.begin(), lim.allowed_slots.end(), a.slot) ==
          lim.allowed_slots.end()) {
        std::string allowed;
        for (auto s : lim.allowed_slots) {
          allowed += (allowed.empty() ? "" : ", ") + std::to_string(s);
        }
        issue(ManifestIssueCode::kSlotNotAllowed, a.slot,
              m.id.str() + " is only allowed in slots " + allowed + " on " + board.id.str() +
                  (lim.reason.empty() ? "" : " (" + lim.reason + ")"));
      }
    }

    if (m.needs_wake && !slot->wake) {
      issue(ManifestIssueCode::kMissingWake, a.slot, m.id.str() + " needs a wake line");
    }
    auto need_bus = [&](bool needed, bool available, const char* bus) {
      if (needed && !available) {
        issue(ManifestIssueCode::kMissingBus, a.slot,
              m.id.str() + " needs " + bus + " on " + slot_str(a.slot));
      }
    };
    need_bus(!m.i2c_addresses.empty(), slot->i2c_branch.has_value(), "i2c");
    need_bus(m.needs_spi, slot->spi, "spi");
    need_bus(m.needs_uart, slot->uart, "uart");
    need_bus(m.needs_analog, slot->analog, "analog");

    for (const auto& load : m.loads) {
      bool routed = false;
      for (const auto& rn : slot->rails) routed |= rn == load.rail;
      const RailSpec* rail = find_rail(board, load.rail);
      if (!routed || !rail) {
        issue(ManifestIssueCode::kRailNotRouted, a.slot,
              "rail " + load.rail + " is not available on " + slot_str(a.slot));
        continue;
      }
      if (load.active_ma > rail->per_slot_max_ma) {
        issue(ManifestIssueCode::kRailSlotLimit, a.slot,
              m.id.str() + " draws " + std::to_string(load.active_ma) + " mA on " + load.rail +
                  ", per-slot limit " + std::to_string(rail->per_slot_max_ma) + " mA");
      }
    }
  }

  // Shared I2C branches: identical addresses collide.
  std::map<std::pair<std::uint8_t, std::uint8_t>, SlotIndex> address_owner;
  for (const auto& p : placed) {
    if (!p.slot->i2c_branch) continue;
    for (auto addr : p.module->i2c_addresses) {
      auto [it, inserted] =
          address_owner.emplace(std::pair{*p.slot->i2c_branch, addr}, p.slot->index);
      if (!inserted) {
        issue(ManifestIssueCode::kI2cAddressConflict, p.slot->index,
              "i2c address " + std::to_string(addr) + " already used by " + slot_str(it->second) +
                  " on the same branch");
      }
    }
  }

  // Enabled mux branches join the root bus: root addresses are off limits.
  for (const auto& p : placed) {
    for (auto addr : p.module->i2c_addresses) {
      for (auto reserved : board.reserved_i2c_addresses) {
        if (addr == reserved) {
          issue(ManifestIssueCode::kI2cRootConflict, p.slot->index,
                p.module->id.str() + " uses i2c address " + std::to_string(addr) +
                    ", reserved on the board's root bus");
        }
      }
    }
  }

  // Card UARTs share a limited number of controller UARTs (HW-MF-005). Test
  // fixtures are exempt: the firmware routes the mux to one of them at a time.
  if (board.uart_channels > 0) {
    std::uint32_t uart_users = 0;
    for (const auto& p : placed) {
      uart_users += p.module->needs_uart && !p.module->test_fixture ? 1 : 0;
    }
    if (uart_users > board.uart_channels) {
      issue(ManifestIssueCode::kUartOversubscribed, std::nullopt,
            std::to_string(uart_users) + " modules need a UART, board provides " +
                std::to_string(board.uart_channels));
    }
  }

  // Rail totals.
  for (const auto& rail : board.rails) {
    std::uint64_t total = 0;
    for (const auto& p : placed) {
      for (const auto& load : p.module->loads) {
        if (load.rail == rail.name) total += load.active_ma;
      }
    }
    if (total > rail.max_ma) {
      issue(ManifestIssueCode::kRailTotalLimit, std::nullopt,
            "rail " + rail.name + " total " + std::to_string(total) + " mA exceeds " +
                std::to_string(rail.max_ma) + " mA");
    }
  }

  // Power-source budget: a connector fitting is not the same as being powerable.
  const PowerSourceBudget* budget = nullptr;
  for (const auto& b : board.power_sources) {
    if (b.source == active_source) budget = &b;
  }
  if (!budget) {
    issue(ManifestIssueCode::kPowerSourceUnsupported, std::nullopt,
          "board does not support power source " + std::string(to_string(active_source)));
    return r;
  }
  // Sustained budget in microwatts: sleepy nodes live below 1 mW.
  auto module_uw = [](const ModuleManifest& m) -> std::uint64_t {
    return m.average_uw != 0 ? m.average_uw : std::uint64_t{m.average_mw} * 1000;
  };
  const std::uint64_t budget_uw =
      budget->sustained_uw != 0 ? budget->sustained_uw : std::uint64_t{budget->sustained_mw} * 1000;
  std::uint64_t avg_uw = 0, peak = 0;
  for (const auto& p : placed) {
    avg_uw += module_uw(*p.module);
    peak += p.module->peak_mw;
  }
  if (avg_uw > budget_uw) {
    std::string heavy;
    for (const auto& p : placed) {
      if (module_uw(*p.module) > budget_uw / 2) heavy += " " + p.module->id.str();
    }
    issue(ManifestIssueCode::kSustainedPowerExceeded, std::nullopt,
          "average " + std::to_string(avg_uw) + " uW exceeds " +
              std::string(to_string(active_source)) + " sustained budget " +
              std::to_string(budget_uw) + " uW" + (heavy.empty() ? "" : "; dominant:" + heavy));
  }
  if (peak > budget->peak_mw) {
    issue(ManifestIssueCode::kPeakPowerExceeded, std::nullopt,
          "combined peak " + std::to_string(peak) + " mW exceeds " +
              std::string(to_string(active_source)) + " peak budget " +
              std::to_string(budget->peak_mw) + " mW");
  }
  return r;
}

}  // namespace talos
