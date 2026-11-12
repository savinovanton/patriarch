#pragma once

#include "pat/core/result.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/data/content_registry.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/tables.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pat::content {

struct TradeNodeDef {
    std::uint16_t slot = 0;
    std::string key;
    std::string name_key;
};

struct HoldingDef {
    std::string province;
    sim::economy::ProvinceKind kind = sim::economy::ProvinceKind::Rural;
    sim::economy::OwnerKind owner_kind = sim::economy::OwnerKind::Unowned;
    std::string owner;
    std::uint16_t trade_node = 0;
    std::uint8_t development = 1;
    std::uint8_t slots_total = 0;
    std::array<std::uint32_t, sim::economy::kGroupCount> population{};
    std::array<core::Fixed32, sim::economy::kGroupCount> mood{};
    std::array<core::Fixed32, sim::economy::kGroupCount> wealth{};
    std::array<core::Fixed32, sim::economy::kMaxGoods> capacity{};
    core::Fixed32 control;
    core::Fixed32 corruption;
    core::Fixed32 unrest;
    core::Fixed32 grain_stock;
    std::vector<std::uint8_t> buildings;
};

/// Scenario bootstrap data. Empty when all three scenario kinds are absent (legacy definition roots).
/// Holdings are ordered by province key; trade nodes by stable slot, and treasuries by dynasty key.
struct M1EconomyContent {
    std::vector<HoldingDef> holdings;
    std::vector<TradeNodeDef> trade_nodes;
    sim::economy::Kopecks crown_treasury_kop = 0;
    std::vector<std::pair<std::string, sim::economy::Kopecks>> dynasty_treasuries;
};

using M1EconomyContentResult = core::Result<M1EconomyContent, std::vector<data::LoadError>>;

/// Resolves the typed holding/trade_node/economy_start objects after schema and reference validation.
/// Checks complete province coverage, map keys, owner and building restrictions without changing World.
[[nodiscard]] M1EconomyContentResult build_m1_economy_content(const data::ContentRegistry& registry,
                                                              const sim::economy::EconomyRules& rules);

}  // namespace pat::content
