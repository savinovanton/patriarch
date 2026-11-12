#pragma once

#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/entity_ids.hpp"
#include "pat/sim/world.hpp"
#include "pat/ui/hud_snapshot.hpp"

#include <cstdint>
#include <functional>
#include <string>

// The bridge from the economy of a World to the heads-up display (E9-T20, US-9.9). Read-only: it takes a
// const World and returns plain data, it never changes the world. All money formatting from kopecks lives
// here: the snapshot carries whole roubles, the window adds the group separators and the sign ("12 480 ₽").

namespace pat::game {

/// Whole roubles of an amount of kopecks, truncated toward zero (-50 kopecks are 0 roubles, 12 480 rubles 99
/// kopecks are 12 480).
[[nodiscard]] std::int64_t kopecks_to_roubles(sim::economy::Kopecks amount) noexcept;

/// Optional inputs of make_hud that the world does not hold.
struct HudOptions {
    /// The rules the simulation runs with. With them the monthly change of the grain is the change of the
    /// granaries over one month at the CURRENT rates (the same functions as the simulation, like the treasury
    /// forecast); without them the change is 0 (the world keeps no memory of last month's stock).
    const sim::economy::EconomyRules* rules = nullptr;
    /// Name of a province for the list of holdings; with no function the name stays empty and the window
    /// falls back to "Province N" by `HudHolding::id`.
    std::function<std::string(sim::ProvinceId)> province_name;
};

/// "Шереметевы" -> "Шереметевых", "Одоевские" -> "Одоевских" (the house in the genitive, "РОД ШЕРЕМЕТЕВЫХ").
[[nodiscard]] std::string clan_genitive(const std::string& clan);

/// The numbers of the top bar and the holdings of dynasty `player`, from the tables of the world:
///   treasury - the balance in roubles and the change of the last closed month (0 before the first close);
///   militia  - the manpower reserve of the dynasty (the "army" until E16), "of" the reserve plus the recruit
///              pools of its provinces (what a full conscription would give);
///   grain    - the sum of the grain stocks of its provinces and its change per month (HudOptions::rules);
///   holdings - its provinces in the order of World::provinces_of, the population in thousands (truncated).
/// Prestige, influence and piety keep the placeholder numbers of make_test_hud() until E7/E31.
/// A world without the economy, an unknown dynasty or an invalid id gives make_test_hud() unchanged.
[[nodiscard]] ui::HudSnapshot
make_hud(const sim::World& world, sim::DynastyId player, const HudOptions& options = {});

/// The player of the spike until the game has one: the owner of the first owned province (invalid if none).
[[nodiscard]] sim::DynastyId first_owner(const sim::World& world);

}  // namespace pat::game
