#pragma once

#include "pat/sim/economy/rules.hpp"
#include "pat/sim/entity_ids.hpp"

#include <cstdint>

// Policies of the dynasties (E9-T12, docs/dev/economy-model.md sections 8 and 17.7, GDD 03 section 9).
//
// A dynasty chooses a tax policy (4 levels in M1) and a recruit policy (3 levels) for ALL its
// provinces; the commands SetTaxPolicy and SetRecruitPolicy change the choice of the dynasty and
// nothing else (World::set_dynasty_tax_policy). The systems read the level a province is processed at
// through the functions below, at the moment the province is handled, so a command takes effect in the
// next monthly pass of each province (the stagger of section 7.2), never in the middle of a month.
//
// The level of a province, in this order:
//   1. a decree of the province itself (ProvinceEconomy::tax_policy / recruit_policy, when it is not
//      kPolicyDefault): reserved for scenarios, no command writes it;
//   2. the choice of the owner dynasty, when the province is held by a dynasty and it has chosen;
//   3. the `default_level` of the rules ("usual"): a dynasty that never chose, a province of the
//      crown, the Church, nobody or free people (imperial policies are the business of E12).
// So a province that changes hands follows its new owner at once, with no command and no bookkeeping.

namespace pat::sim {
class World;
}

namespace pat::sim::economy {

/// Tells the world how many levels the policies of `rules` have, which is the range the commands
/// SetTaxPolicy and SetRecruitPolicy accept (World::set_economy_policy_levels). The system
/// `economy.provinces` does it every day, so a running game always follows its rules; call it
/// yourself to have the range right before the first tick (scenario builders, tests). A change to a
/// number that is already set is a no-op. Without economy: nothing.
void bind_policy_levels(World& world, const EconomyRules& rules) noexcept;

/// The tax policy level the province is processed at; unknown province or no economy: the default.
[[nodiscard]] std::uint8_t
effective_tax_level(const World& world, const EconomyRules& rules, ProvinceId province) noexcept;
/// The recruit policy level the province is processed at; same rules.
[[nodiscard]] std::uint8_t
effective_recruit_level(const World& world, const EconomyRules& rules, ProvinceId province) noexcept;

}  // namespace pat::sim::economy
