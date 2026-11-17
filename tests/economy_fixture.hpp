#pragma once

// Shared economy rules and simulation helpers for tests (header-only). `make_world` stays in each
// test: the worlds differ by the task of the test.

#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <memory>
#include <utility>

namespace pat::test {

/// The built-in default economy rules.
inline const sim::economy::EconomyRules& default_rules() {
    static const sim::economy::EconomyRules value = sim::economy::EconomyRules::defaults();
    return value;
}

/// The defaults with the dynamics of the administration (control, corruption), mood, unrest and riots
/// frozen: what a step reads does not depend on history, so a test sees only what it sets. The dynamics
/// have their own tests (economy_administrator_test, economy_mood_test, economy_unrest_test).
inline const sim::economy::EconomyRules& frozen_admin_rules() {
    static const sim::economy::EconomyRules value = [] {
        sim::economy::EconomyRules r = sim::economy::EconomyRules::defaults();
        r.control.control_inertia_permille = 0;
        r.control.corruption_inertia_permille = 0;
        r.mood.inertia_permille = 0;
        r.unrest.inertia_permille = 0;
        r.unrest.riot_chance_min_permille = 0;
        r.unrest.riot_chance_max_permille = 0;
        return r;
    }();
    return value;
}

/// A simulation over `world` with the economy systems installed under `rules` (and, optionally, a
/// price source instead of the static prices of the rules).
inline sim::Simulation economy_sim(sim::World world,
                                   const sim::economy::EconomyRules& rules,
                                   std::shared_ptr<const sim::economy::PriceSource> prices = nullptr) {
    sim::Simulation simulation(std::move(world));
    sim::economy::install_economy_systems(simulation, rules, std::move(prices));
    return simulation;
}

}  // namespace pat::test
