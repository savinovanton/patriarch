#pragma once

#include "pat/core/fixed.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/types.hpp"
#include "pat/sim/entity_ids.hpp"

#include <array>
#include <cstdint>

// Monthly growth of the population (E9-T06, docs/dev/economy-model.md section 9 formula F1 and
// section 7.3 step 2, GDD 03 section 4.2).
//
// Everything here is a pure function of its arguments, integer-only (no float, no random numbers):
//
//   * compute_group_growth   - one group: the yearly net rate with its Breakdown, the whole people
//                              added or removed this month, and the new remainder;
//   * compute_province_growth - the six groups of a province (reads the World, changes nothing);
//   * apply_province_growth  - writes a result into the World (sizes through World::set_group_size so
//                              the population cache stays right, remainders through the columns);
//   * step_province_population - compute + apply, what the system `economy.provinces` calls on the
//                              slot day of a province.
//
// The rate (ppm of the group per YEAR):
//   net = birth - death + (wealth - 100) x wealth_growth_ppm_per_point + growth_modifier
//   if net > 0: net x= crowding / 1000, crowding = clamp((cap - persons) x 1000 / cap, 0, 1000),
//               cap = capacity_per_development x development, persons = people in the province
//               (a crowded province slows growth; shrinkage is never slowed)
// The month:
//   acc = carry + size x net / 12                 (micro-people; net is ppm per year)
//   delta = acc / 10^6 truncated toward zero      (whole people)
//   carry' = acc % 10^6                           (same sign as acc, |carry'| < 10^6)
//   size' = clamp(size + delta - losses, 0, ...)  (a group cannot go below nobody)
// So a rate too small to move a whole person in one month is not lost: it piles up in the carry
// until it is. The only rounding is the truncation of `size x net / 12` to a micro-person, which is
// deterministic and below 10^-6 of a person per group per month.
//
// `losses` (people) is the interface for everything that removes people from a group in the same
// step - recruits (E9-T13), epidemics and famine (M2). The system passes 0 for now.

namespace pat::sim {
class World;
}

namespace pat::sim::economy {

/// Micro-people in one person: the unit of the growth remainder.
inline constexpr std::int64_t kMicroPerPerson = 1'000'000;
/// Months in a year; the yearly rate is spread evenly over them.
inline constexpr std::int64_t kMonthsPerYear = 12;
/// The "normal" wealth: at this value wealth neither helps nor hinders growth.
inline constexpr std::int32_t kNormalWealthPoints = 100;

/// What F1 needs to know about one group of one province.
struct GrowthInput {
    Group group = Group::Peasants;
    std::uint32_t size = 0;  ///< people now
    std::int32_t carry = 0;  ///< micro-people left over from earlier months
    core::Fixed32 wealth = core::Fixed32::from_int(kNormalWealthPoints);
    std::int32_t growth_modifier_ppm = 0;  ///< ppm per year (events, policies)
    std::uint64_t province_persons = 0;    ///< people in all groups of the province (crowding)
    std::int32_t development = 1;          ///< 1..kMaxDevelopment (capacity)
    std::uint32_t losses = 0;              ///< people removed this month (recruits, epidemics); 0 for now
};

struct GrowthResult {
    /// The yearly net rate in ppm after crowding: the total of the Breakdown.
    std::int32_t net_ppm_year = 0;
    /// Crowding factor applied to a positive rate, per-mille (1000 = no slowing, also for shrinkage).
    std::int32_t crowding_permille = 1000;
    /// Whole people born minus died this month (before losses), signed.
    std::int64_t delta_people = 0;
    /// People actually removed by `losses` (never more than there were).
    std::uint32_t lost_people = 0;
    std::uint32_t new_size = 0;
    std::int32_t new_carry = 0;

    [[nodiscard]] bool operator==(const GrowthResult&) const = default;
};

/// Growth of one group in one month. With `breakdown` the yearly net rate is explained: the root
/// (code term::kGrowthNet, unit Ppm) is the rate after crowding, its children are birth (+), death (-),
/// wealth, modifier and the crowding step (a non-positive delta; factor_permille = crowding). Terms
/// that are 0 are left out. The result never depends on whether a breakdown was requested.
[[nodiscard]] GrowthResult
compute_group_growth(const PopulationRules& rules, const GrowthInput& input, Breakdown* breakdown = nullptr);

/// The input of group `group` of province `province` as the World holds it (losses 0).
/// `province_persons` is the total before any group of the province changes this month.
[[nodiscard]] GrowthInput growth_input(const World& world, ProvinceId province, Group group);

/// The six groups of a province.
struct ProvinceGrowth {
    std::array<GrowthResult, kGroupCount> groups{};
    std::uint64_t persons_before = 0;
    std::uint64_t persons_after = 0;

    [[nodiscard]] bool operator==(const ProvinceGrowth&) const = default;
};

/// Growth of every group of a province, computed from the state of the start of the month (so the
/// result does not depend on the order of the groups). `breakdowns` (optional) points to
/// kGroupCount Breakdowns, one per group. Reads the World only; nothing is written.
[[nodiscard]] ProvinceGrowth compute_province_growth(const World& world,
                                                     const PopulationRules& rules,
                                                     ProvinceId province,
                                                     Breakdown* breakdowns = nullptr);

/// Writes a computed growth into the World: sizes (World::set_group_size, which also refreshes the
/// population cache and keeps the total at kMaxProvincePersons) and remainders. Groups that did not
/// change are not touched.
void apply_province_growth(World& world, ProvinceId province, const ProvinceGrowth& growth);

/// compute_province_growth + apply_province_growth; returns what was applied.
ProvinceGrowth step_province_population(World& world, const PopulationRules& rules, ProvinceId province);

/// The explanation of the yearly net rate of one group, as a tooltip shows it (the same code path as
/// the simulation: a Breakdown filled by compute_group_growth).
[[nodiscard]] Breakdown
explain_group_growth(const World& world, const PopulationRules& rules, ProvinceId province, Group group);

}  // namespace pat::sim::economy
