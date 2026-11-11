#pragma once

#include "pat/sim/economy/rules.hpp"

#include <string>
#include <vector>

// Level R of the economy invariants (docs/dev/economy-model.md section 12, decision D13): the checks that
// need the RULES the world is played with and so cannot live in `World::check_invariants()` (level K, which
// knows only the world). A save made with other rules, a scenario that names a building or a policy level
// the rules do not define, a hand-edited column: K lets them through and the formulas silently substitute
// the last level or a zero upkeep; R reports them.
//
// A pure read of the world: no changes, no random numbers, deterministic order of the findings. An empty
// result means the world is consistent with the rules. A world without economy has nothing to check.
//
//   E-R1  every province and every dynasty: tax_policy < tax.level_count and recruit_policy <
//         recruit.level_count, or "not chosen" (kPolicyDefault, E9-T12); the numbers of levels the
//         world was given (set_economy_policy_levels), when set, equal those of the rules; every
//         built slot (below slots_total) holds a building type the rules define; every entry of the
//         construction queue builds such a type; goods are the fixed array of kMaxGoods and need no check;
//         for a province that the monthly slot already processed (last_slot_date set):
//         grain_stock <= stock_cap_months x the need of its people (the cap the slot applies).
//         The cap is not checked for a province that was never processed (a scenario sets its stock) and
//         may be exceeded between a direct write of the group sizes and the next slot: call the check after
//         a tick, not in the middle of hand edits; for the same provinces (E9-T13, F11):
//         recruit_pool <= recruit_ceiling, the potential the people give under the heaviest recruit
//         policy and full control, with no quota. The cap of the policy of the day is exact only right
//         after a slot (the slot cuts the pool to it); a command or a fall of control between two slots
//         can lower it below the pool for a few days, so the invariant uses the bound that no command
//         can break.
//   E-L1  (rules part) no dynasty holds more loans than loans.max_loans_per_dynasty.
//
// E-R2 (the postings of a province's tax add up to the lines of the statement) is a TEST (ECO-A1), not a
// function: it needs the income of the slot, which the world does not keep.
//
// `World::check_invariants()` stays the one for saves and loads; this one is for tests, tools and
// `contentlint` scenarios.

namespace pat::sim {
class World;
}

namespace pat::sim::economy {

[[nodiscard]] std::vector<std::string> check_rules_invariants(const World& world, const EconomyRules& rules);

}  // namespace pat::sim::economy
