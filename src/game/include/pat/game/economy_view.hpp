#pragma once

#include "pat/loc/localizer.hpp"
#include "pat/sim/economy/administrator.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/settlement.hpp"
#include "pat/sim/world.hpp"
#include "pat/ui/economy_snapshot.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace pat::game {

struct EconomyViewOptions {
    const sim::economy::PriceSource* prices = nullptr;
    const sim::economy::ExpenseSources* expenses = nullptr;
    const sim::economy::AdministratorSource* administrators = nullptr;
    std::function<std::string(sim::ProvinceId)> province_name;
    std::function<std::string(sim::DynastyId)> dynasty_name;
    std::function<std::string(sim::CharacterId)> character_name;
};

/// Recorded state gets a one-term explanation; computed queries retain the simulation breakdown.
[[nodiscard]] ui::EconomyNumber recorded_economy_value(const loc::Localizer& loc,
                                                       ui::EconomyUnit unit,
                                                       std::int64_t value,
                                                       std::string_view label_key);
[[nodiscard]] ui::EconomyNumber projected_economy_value(const loc::Localizer& loc,
                                                        const sim::economy::Breakdown& breakdown,
                                                        std::string_view label_key);

[[nodiscard]] ui::ProvinceEconomySnapshot province_economy_snapshot(const loc::Localizer& loc,
                                                                    const sim::World& world,
                                                                    sim::ProvinceId province,
                                                                    const sim::economy::EconomyRules& rules,
                                                                    const EconomyViewOptions& options = {});

[[nodiscard]] ui::TreasurySnapshot treasury_snapshot(const loc::Localizer& loc,
                                                     const sim::World& world,
                                                     sim::economy::AccountRef account,
                                                     const sim::economy::EconomyRules& rules,
                                                     const EconomyViewOptions& options = {});

}  // namespace pat::game
