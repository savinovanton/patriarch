#include "pat/sim/economy/mood.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/economy/administrator.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/policy.hpp"
#include "pat/sim/economy/population.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>

namespace pat::sim::economy {

using core::Fixed32;

namespace {

constexpr std::int64_t kOne = Fixed32::one_raw;
constexpr std::int64_t kMoodMaxRaw = 100 * kOne;
constexpr std::int64_t kMoodMinRaw = -100 * kOne;

Fixed32 from_raw64(std::int64_t raw) noexcept {
    return Fixed32::from_raw(static_cast<std::int32_t>(raw));
}

}  // namespace

// ---- F8 -------------------------------------------------------------------------------------------

Fixed32 tax_mood_pressure(const TaxRules& tax, std::uint8_t level, Group group) noexcept {
    const std::size_t g = index_of(group);
    if (tax.level_count <= 0 || g >= kGroupCount) {
        return Fixed32::zero();
    }
    const auto last = static_cast<std::size_t>(tax.level_count - 1);
    return tax.levels[std::min<std::size_t>(level, last)].mood_pressure[g];
}

Fixed32 recruit_mood_pressure(const RecruitRules& recruit, std::uint8_t level, Group group) noexcept {
    const std::size_t g = index_of(group);
    if (recruit.level_count <= 0 || g >= kGroupCount) {
        return Fixed32::zero();
    }
    const auto last = static_cast<std::size_t>(recruit.level_count - 1);
    return recruit.levels[std::min<std::size_t>(level, last)].mood_pressure[g];
}

Fixed32
compute_group_target(const EconomyRules& rules, const MoodInput& input, Group group, Breakdown* breakdown) {
    const std::size_t g = index_of(group);
    if (g >= kGroupCount) {
        return Fixed32::zero();
    }
    const MoodRules& mood = rules.mood;
    const std::int64_t base = mood.base_mood[g].raw();
    const std::int64_t tax =
        -static_cast<std::int64_t>(tax_mood_pressure(rules.tax, input.tax_level, group).raw());
    const std::int64_t recruit =
        -static_cast<std::int64_t>(recruit_mood_pressure(rules.recruit, input.recruit_level, group).raw());
    const std::int64_t wealth = mul_permille(static_cast<std::int64_t>(input.wealth[g].raw()) -
                                                 static_cast<std::int64_t>(kNormalWealthPoints) * kOne,
                                             mood.wealth_slope_permille);
    const std::int64_t administrator =
        mul_permille(static_cast<std::int64_t>(input.control.raw()) - kNeutralControlPoints * kOne,
                     mood.admin_slope_permille);
    const std::int64_t events = input.events[g].raw();
    const std::int64_t sum = base + tax + recruit + wealth + administrator + events;
    const std::int64_t target = std::clamp(sum, kMoodMinRaw, kMoodMaxRaw);

    if (breakdown != nullptr) {
        breakdown->begin(term::kMoodTarget, Unit::Points, target);
        (void)breakdown->add_term(kRootNode, term::kMoodBase, base);
        if (tax != 0) {
            (void)breakdown->add_term(kRootNode, term::kMoodTaxPressure, tax);
        }
        if (recruit != 0) {
            (void)breakdown->add_term(kRootNode, term::kMoodRecruitPressure, recruit);
        }
        if (wealth != 0) {
            (void)breakdown->add_term(kRootNode, term::kMoodWealth, wealth, mood.wealth_slope_permille);
        }
        if (administrator != 0) {
            (void)breakdown->add_term(
                kRootNode, term::kMoodAdministrator, administrator, mood.admin_slope_permille);
        }
        if (events != 0) {
            (void)breakdown->add_term(kRootNode, term::kMoodEvents, events);
        }
        if (target != sum) {
            (void)breakdown->add_term(kRootNode, term::kMoodLimit, target - sum);
        }
        PAT_ASSERT(breakdown->is_consistent(), "the mood breakdown must add up to the target");
    }
    return from_raw64(target);
}

GroupMood
compute_group_mood(const EconomyRules& rules, const MoodInput& input, Group group, Breakdown* breakdown) {
    GroupMood out;
    const std::size_t g = index_of(group);
    if (g >= kGroupCount) {
        return out;
    }
    out.target = compute_group_target(rules, input, group, breakdown);
    // a column that was written out of its range is pulled back into it here
    const Fixed32 current =
        from_raw64(std::clamp<std::int64_t>(input.mood[g].raw(), kMoodMinRaw, kMoodMaxRaw));
    out.mood = approach(current, out.target, rules.mood.inertia_permille);
    PAT_ASSERT(out.mood.raw() >= kMoodMinRaw && out.mood.raw() <= kMoodMaxRaw,
               "mood must stay within -100..100");
    return out;
}

ProvinceMood compute_province_mood(const EconomyRules& rules, const MoodInput& input) {
    ProvinceMood out;
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        out.group[g] = compute_group_mood(rules, input, static_cast<Group>(g));
    }
    return out;
}

// ---- province --------------------------------------------------------------------------------------

MoodInput mood_input(const World& world, const EconomyRules& rules, ProvinceId province) {
    MoodInput in;
    if (!world.economy_present() || !world.exists(province)) {
        return in;
    }
    const EconomyState& e = world.economy();
    const std::size_t p = province.index();
    const std::size_t first = p * kGroupCount;
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        in.mood[g] = e.groups.mood[first + g];
        in.wealth[g] = e.groups.wealth[first + g];
    }
    in.control = e.provinces.control[p];
    in.tax_level = effective_tax_level(world, rules, province);
    in.recruit_level = effective_recruit_level(world, rules, province);
    return in;
}

void apply_province_mood(World& world, ProvinceId province, const ProvinceMood& result) {
    if (!world.economy_present() || !world.exists(province)) {
        return;
    }
    EconomyColumnsMut columns = world.economy_columns_mut();
    const std::size_t first = province.index() * kGroupCount;
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        columns.mood[first + g] = result.group[g].mood;
    }
}

ProvinceMood step_province_mood(World& world, const EconomyRules& rules, ProvinceId province) {
    if (!world.economy_present() || !world.exists(province)) {
        return {};
    }
    const ProvinceMood result = compute_province_mood(rules, mood_input(world, rules, province));
    apply_province_mood(world, province, result);
    return result;
}

// ---- explanations ----------------------------------------------------------------------------------

Breakdown explain_mood(const EconomyRules& rules, const MoodInput& input, Group group) {
    Breakdown out;
    (void)compute_group_target(rules, input, group, &out);
    return out;
}

}  // namespace pat::sim::economy
