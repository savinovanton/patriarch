#pragma once

// Shared world checks for tests (header-only).

#include "pat/sim/economy/invariants.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

namespace pat::test {

/// Fails the test (printing every problem) unless the world passes check_invariants().
inline void require_valid(const sim::World& world) {
    const auto problems = world.check_invariants();
    for (const auto& problem : problems) {
        MESSAGE(problem);
    }
    REQUIRE(problems.empty());
}

/// require_valid(world) plus the economy rule invariants (check_rules_invariants) under `rules`.
inline void require_valid(const sim::World& world, const sim::economy::EconomyRules& rules) {
    require_valid(world);
    const auto broken = sim::economy::check_rules_invariants(world, rules);
    for (const auto& problem : broken) {
        MESSAGE(problem);
    }
    REQUIRE(broken.empty());
}

}  // namespace pat::test
