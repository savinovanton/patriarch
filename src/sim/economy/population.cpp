#include "pat/sim/economy/population.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <limits>

namespace pat::sim::economy {

namespace {

/// The wealth term of the yearly rate: (wealth - 100) points x ppm per point, truncated toward zero
/// at the resolution of Fixed32 (1/65536 point).
std::int64_t wealth_term_ppm(const PopulationRules& rules, core::Fixed32 wealth) noexcept {
    const std::int64_t above_normal = static_cast<std::int64_t>(wealth.raw()) -
                                      static_cast<std::int64_t>(kNormalWealthPoints) * core::Fixed32::one_raw;
    return above_normal * rules.wealth_growth_ppm_per_point / core::Fixed32::one_raw;
}

/// 1000 when the province has room, falling linearly to 0 when its people fill the capacity.
std::int32_t
crowding_permille(const PopulationRules& rules, std::uint64_t persons, std::int32_t development) noexcept {
    const std::int64_t dev = std::clamp<std::int64_t>(development, 1, kMaxDevelopment);
    const std::int64_t cap = static_cast<std::int64_t>(rules.capacity_per_development) * dev;
    if (cap <= 0 || persons >= static_cast<std::uint64_t>(cap)) {
        return 0;
    }
    const std::int64_t room = cap - static_cast<std::int64_t>(persons);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(room * kPermilleOne / cap, 0, kPermilleOne));
}

}  // namespace

GrowthResult
compute_group_growth(const PopulationRules& rules, const GrowthInput& input, Breakdown* breakdown) {
    const std::size_t g = index_of(input.group);
    PAT_ASSERT(g < kGroupCount, "growth of an unknown group");
    const std::int64_t birth = rules.birth_ppm_year[g];
    const std::int64_t death = rules.death_ppm_year[g];
    const std::int64_t wealth = wealth_term_ppm(rules, input.wealth);
    const std::int64_t modifier = input.growth_modifier_ppm;
    const std::int64_t before_crowding = birth - death + wealth + modifier;

    GrowthResult out;
    out.crowding_permille = 1000;
    std::int64_t net = before_crowding;
    if (before_crowding > 0) {
        out.crowding_permille = crowding_permille(rules, input.province_persons, input.development);
        net = before_crowding * out.crowding_permille / kPermilleOne;
    }
    // The rules are validated (rates <= 10^6 ppm, wealth term <= 2 x 10^7), so |net| < 2^31.
    out.net_ppm_year = static_cast<std::int32_t>(std::clamp<std::int64_t>(
        net, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));

    if (breakdown != nullptr) {
        breakdown->begin(term::kGrowthNet, Unit::Ppm, out.net_ppm_year);
        (void)breakdown->add_term(kRootNode, term::kGrowthBirth, birth);
        (void)breakdown->add_term(kRootNode, term::kGrowthDeath, -death);
        if (wealth != 0) {
            (void)breakdown->add_term(kRootNode, term::kGrowthWealth, wealth);
        }
        if (modifier != 0) {
            (void)breakdown->add_term(kRootNode, term::kGrowthModifier, modifier);
        }
        if (net != before_crowding) {
            (void)breakdown->add_step(
                kRootNode, term::kGrowthCrowding, before_crowding, net, out.crowding_permille);
        }
        PAT_ASSERT(breakdown->is_consistent(), "the growth breakdown must add up to the net rate");
    }

    // acc is at most 2^32 x 2^31 / 12 + 10^6 < 2^61, no overflow.
    const std::int64_t acc = static_cast<std::int64_t>(input.carry) +
                             static_cast<std::int64_t>(input.size) * out.net_ppm_year / kMonthsPerYear;
    out.delta_people = acc / kMicroPerPerson;
    out.new_carry = static_cast<std::int32_t>(acc % kMicroPerPerson);

    const std::int64_t grown = static_cast<std::int64_t>(input.size) + out.delta_people;
    const std::int64_t after_growth =
        std::clamp<std::int64_t>(grown, 0, std::numeric_limits<std::uint32_t>::max());
    const std::uint32_t lost = static_cast<std::uint32_t>(std::min<std::int64_t>(input.losses, after_growth));
    out.lost_people = lost;
    out.new_size = static_cast<std::uint32_t>(after_growth - lost);
    return out;
}

GrowthInput growth_input(const World& world, ProvinceId province, Group group) {
    GrowthInput in;
    in.group = group;
    if (!world.economy_present() || !world.exists(province)) {
        return in;
    }
    const EconomyState& e = world.economy();
    const std::size_t row = group_row(province.index(), group);
    in.size = e.groups.size[row];
    in.carry = e.groups.growth_carry[row];
    in.wealth = e.groups.wealth[row];
    in.growth_modifier_ppm = e.groups.growth_modifier[row];
    in.province_persons = world.population_persons(province);
    in.development = world.provinces().development[province.index()];
    return in;
}

ProvinceGrowth compute_province_growth(const World& world,
                                       const PopulationRules& rules,
                                       ProvinceId province,
                                       Breakdown* breakdowns) {
    ProvinceGrowth out;
    if (!world.economy_present() || !world.exists(province)) {
        return out;
    }
    const EconomyState& e = world.economy();
    const std::size_t first = province.index() * kGroupCount;
    const std::uint64_t persons = world.population_persons(province);
    const std::int32_t development = world.provinces().development[province.index()];
    out.persons_before = persons;
    std::uint64_t after = 0;
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        const std::size_t row = first + g;
        GrowthInput in;
        in.group = static_cast<Group>(g);
        in.size = e.groups.size[row];
        in.carry = e.groups.growth_carry[row];
        in.wealth = e.groups.wealth[row];
        in.growth_modifier_ppm = e.groups.growth_modifier[row];
        in.province_persons = persons;
        in.development = development;
        Breakdown* breakdown = breakdowns != nullptr ? &breakdowns[g] : nullptr;
        out.groups[g] = compute_group_growth(rules, in, breakdown);
        after += out.groups[g].new_size;
    }
    out.persons_after = after;
    return out;
}

void apply_province_growth(World& world, ProvinceId province, const ProvinceGrowth& growth) {
    if (!world.economy_present() || !world.exists(province)) {
        return;
    }
    const EconomyColumnsMut columns = world.economy_columns_mut();
    const std::size_t first = province.index() * kGroupCount;
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        const GrowthResult& r = growth.groups[g];
        columns.growth_carry[first + g] = r.new_carry;
        if (world.economy().groups.size[first + g] != r.new_size) {
            (void)world.set_group_size(province, static_cast<Group>(g), r.new_size);
        }
    }
}

ProvinceGrowth step_province_population(World& world, const PopulationRules& rules, ProvinceId province) {
    const ProvinceGrowth growth = compute_province_growth(world, rules, province);
    apply_province_growth(world, province, growth);
    return growth;
}

Breakdown
explain_group_growth(const World& world, const PopulationRules& rules, ProvinceId province, Group group) {
    Breakdown out;
    (void)compute_group_growth(rules, growth_input(world, province, group), &out);
    return out;
}

}  // namespace pat::sim::economy
