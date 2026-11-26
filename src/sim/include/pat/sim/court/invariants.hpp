#pragma once

#include "pat/sim/court_rules.hpp"
#include "pat/sim/world.hpp"

#include <string>
#include <vector>

// The court invariants that need the rules (docs/dev/court-model.md section 12), next to the structural
// ones World::check_invariants() checks without them. E10-T02 covers the parts of C-2, C-4...C-7 and
// C-13 that depend on CourtRules: known office types, servant roles and parties; seats valid for the seat
// rule of their type (fixed seat numbers, crown provinces of the listed kinds); seats of a type within
// its bound; offices per person, postponements, petitions per dynasty, servants per role and services
// within the limits of the balance. C-3 (requirements of the holder) needs the eligibility of E10-T05;
// C-8 (the trait `disgraced`) and the date windows of the seats are checked by the tasks that write them.

namespace pat::sim {

/// Empty when every rule-dependent court invariant holds; otherwise human-readable violations.
[[nodiscard]] std::vector<std::string> check_court_invariants(const World& world, const CourtRules& rules);

}  // namespace pat::sim
