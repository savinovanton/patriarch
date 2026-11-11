#include "pat/core/rng.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/economy/administrator.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/mood.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

// E9-T11: the mood of the population groups (docs/dev/economy-model.md section 9 formula F8, section 7.3
// step 5; GDD 03 section 4.2): the terms, the order, the truncation, the Breakdown, the movement toward
// the target and the single place the shortage of bread reaches the mood (through wealth).

using namespace pat::sim;
using namespace pat::sim::economy;
using pat::core::Fixed32;
using pat::core::Pcg32;
using pat::core::StringId;

namespace {

constexpr std::int64_t kOne = Fixed32::one_raw;

Fixed32 points(std::int64_t whole) {
    return Fixed32::from_int(static_cast<std::int32_t>(whole));
}

Fixed32 raw(std::int64_t value) {
    return Fixed32::from_raw(static_cast<std::int32_t>(value));
}

const EconomyRules& rules() {
    static const EconomyRules value = EconomyRules::defaults();
    return value;
}

/// The default rules with control, unrest and riots standing still, so that a test of the mood sees
/// only the mood move.
EconomyRules mood_only_rules() {
    EconomyRules r = EconomyRules::defaults();
    r.control.control_inertia_permille = 0;
    r.control.corruption_inertia_permille = 0;
    r.unrest.inertia_permille = 0;
    r.unrest.riot_chance_min_permille = 0;
    r.unrest.riot_chance_max_permille = 0;
    r.consumption.wealth_inertia_permille = 0;  // the wealth stays where the test puts it
    return r;
}

/// A neutral province as F8 reads it: normal wealth, control at the neutral 50, and the policy levels
/// that press on nobody (the usual tax, the voluntary recruitment).
MoodInput neutral_input() {
    MoodInput in;
    in.wealth.fill(points(100));
    in.control = points(kNeutralControlPoints);
    in.tax_level = 1;
    in.recruit_level = 0;
    return in;
}

void require_valid(const World& world) {
    const auto problems = world.check_invariants();
    for (const auto& problem : problems) {
        MESSAGE(problem);
    }
    REQUIRE(problems.empty());
}

World make_world(std::uint32_t provinces,
                 std::uint32_t peasants = 1000,
                 Date start = Date::from_ymd(1702, 12, 31)) {
    World world(start, 5);
    (void)world.create_dynasty(StringId(1));
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.create_province(StringId(100 + p), DynastyId(0), Fixed32::from_int(1));
    }
    REQUIRE(world.enable_economy());
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.set_group_size(ProvinceId(p), Group::Peasants, peasants);
        world.economy_columns_mut().tax_policy[p] = static_cast<std::uint8_t>(rules().tax.default_level);
        world.economy_columns_mut().recruit_policy[p] =
            static_cast<std::uint8_t>(rules().recruit.default_level);
    }
    require_valid(world);
    return world;
}

Fixed32 mood_of(const World& world, std::uint32_t province, Group group) {
    return world.economy().groups.mood[group_row(province, group)];
}

}  // namespace

// ---- the terms ----------------------------------------------------------------------------------

TEST_CASE("F8 target: the terms, their sign and their order, hand-computed") {
    EconomyRules r = EconomyRules::defaults();
    r.mood.base_mood[3] = points(4);  // peasants are inclined to be content
    MoodInput in = neutral_input();
    in.tax_level = 3;            // excessive: pressure 14 on the peasants
    in.recruit_level = 2;        // intensified: pressure 8 on the peasants
    in.wealth[3] = points(150);  // +50 over the norm
    in.control = points(70);     // +20 over the neutral 50
    in.events[3] = raw(3 * kOne / 2);

    // 4 - 14 - 8 + 50 x 0.1 + 20 x 0.1 + 1.5 = -9.5 points
    const Fixed32 target = compute_group_target(r, in, Group::Peasants);
    CHECK(target == raw(-19 * kOne / 2));

    Breakdown b;
    CHECK(compute_group_target(r, in, Group::Peasants, &b) == target);
    REQUIRE(b.is_consistent());
    CHECK(b.root().code == term::kMoodTarget);
    CHECK(b.root().unit == Unit::Points);
    CHECK(b.total() == target.raw());
    CHECK(b.node(b.find(term::kMoodBase)).value == 4 * kOne);
    CHECK(b.node(b.find(term::kMoodTaxPressure)).value == -14 * kOne);
    CHECK(b.node(b.find(term::kMoodRecruitPressure)).value == -8 * kOne);
    CHECK(b.node(b.find(term::kMoodWealth)).value == 5 * kOne);
    CHECK(b.node(b.find(term::kMoodWealth)).factor_permille == r.mood.wealth_slope_permille);
    CHECK(b.node(b.find(term::kMoodAdministrator)).value == 2 * kOne);
    CHECK(b.node(b.find(term::kMoodAdministrator)).factor_permille == r.mood.admin_slope_permille);
    CHECK(b.node(b.find(term::kMoodEvents)).value == 3 * kOne / 2);
    // the terms are in the order of the formula
    CHECK(b.find(term::kMoodBase) < b.find(term::kMoodTaxPressure));
    CHECK(b.find(term::kMoodTaxPressure) < b.find(term::kMoodRecruitPressure));
    CHECK(b.find(term::kMoodRecruitPressure) < b.find(term::kMoodWealth));
    CHECK(b.find(term::kMoodWealth) < b.find(term::kMoodAdministrator));
    CHECK(b.find(term::kMoodAdministrator) < b.find(term::kMoodEvents));
    // the terms of M1 that are 0 (coercion, religion) and the clamp are not listed
    CHECK(b.find(term::kMoodCoercion) == kNoNode);
    CHECK(b.find(term::kMoodReligion) == kNoNode);
    CHECK(b.find(term::kMoodLimit) == kNoNode);
}

TEST_CASE("F8 target: a neutral province is at the base, and only the base is listed") {
    const MoodInput in = neutral_input();
    Breakdown b;
    CHECK(compute_group_target(rules(), in, Group::Townsfolk, &b) == Fixed32::zero());
    REQUIRE(b.is_consistent());
    CHECK(b.size() == 2);  // the total and the base
    CHECK(b.node(b.find(term::kMoodBase)).value == 0);
}

TEST_CASE("F8 target: the terms are truncated toward zero, below and above the norm alike") {
    EconomyRules r = EconomyRules::defaults();
    r.mood.wealth_slope_permille = 333;
    r.mood.admin_slope_permille = 7;
    MoodInput in = neutral_input();
    for (const std::int32_t wealth_raw : {-100'001, -1, 1, 99'999, 123'457}) {
        for (const std::int32_t control_raw : {-70'001, -1, 1, 70'001}) {
            in.wealth[3] = raw(100 * kOne + wealth_raw);
            in.control = raw(50 * kOne + control_raw);
            const std::int64_t expected = static_cast<std::int64_t>(wealth_raw) * 333 / 1000 +
                                          static_cast<std::int64_t>(control_raw) * 7 / 1000;
            INFO("wealth " << wealth_raw << " control " << control_raw);
            CHECK(compute_group_target(r, in, Group::Peasants).raw() == expected);
        }
    }
}

TEST_CASE("F8 target: wealth is the wealth of the group itself, not of the province") {
    MoodInput in = neutral_input();
    in.wealth[2] = points(40);   // townsfolk are poor
    in.wealth[3] = points(160);  // peasants are well off
    CHECK(compute_group_target(rules(), in, Group::Townsfolk) == points(-6));
    CHECK(compute_group_target(rules(), in, Group::Peasants) == points(6));
    CHECK(compute_group_target(rules(), in, Group::Clergy) == Fixed32::zero());
}

TEST_CASE("F8 target: the base mood of every group is its own") {
    EconomyRules r = EconomyRules::defaults();
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        r.mood.base_mood[g] = points(static_cast<std::int64_t>(g) - 2);
    }
    const MoodInput in = neutral_input();
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        CHECK(compute_group_target(r, in, static_cast<Group>(g)) == points(static_cast<std::int64_t>(g) - 2));
    }
}

TEST_CASE("F8 target: the limit to -100..100 is a term of its own and the total stays exact") {
    MoodInput in = neutral_input();
    in.events[3] = points(-90);
    in.tax_level = 3;      // -14
    in.recruit_level = 2;  // -8
    Breakdown low;
    CHECK(compute_group_target(rules(), in, Group::Peasants, &low) == points(-100));
    REQUIRE(low.is_consistent());
    // -90 - 14 - 8 = -112: the clamp gives 12 points back
    CHECK(low.node(low.find(term::kMoodLimit)).value == 12 * kOne);

    in.events[3] = points(99);
    in.tax_level = 0;  // the low tax relieves the peasants: +6
    in.recruit_level = 0;
    Breakdown high;
    CHECK(compute_group_target(rules(), in, Group::Peasants, &high) == points(100));
    REQUIRE(high.is_consistent());
    // 99 + 6 = 105: the clamp takes 5 points away
    CHECK(high.node(high.find(term::kMoodLimit)).value == -5 * kOne);
    CHECK(high.node(high.find(term::kMoodTaxPressure)).value == 6 * kOne);
}

TEST_CASE("F8 target: an unknown group or a level past the table is handled, not trusted") {
    MoodInput in = neutral_input();
    in.tax_level = 200;      // clamped to the last level
    in.recruit_level = 200;  // likewise
    const Group unknown = static_cast<Group>(kGroupCount);
    CHECK(compute_group_target(rules(), in, unknown) == Fixed32::zero());
    CHECK(compute_group_mood(rules(), in, unknown) == GroupMood{});
    const int last_tax = rules().tax.level_count - 1;
    const int last_recruit = rules().recruit.level_count - 1;
    CHECK(tax_mood_pressure(rules().tax, 200, Group::Peasants) ==
          rules().tax.levels[static_cast<std::size_t>(last_tax)].mood_pressure[3]);
    CHECK(recruit_mood_pressure(rules().recruit, 200, Group::Peasants) ==
          rules().recruit.levels[static_cast<std::size_t>(last_recruit)].mood_pressure[3]);
    CHECK(tax_mood_pressure(rules().tax, 1, unknown) == Fixed32::zero());
    CHECK(compute_group_target(rules(), in, Group::Peasants) ==
          -(rules().tax.levels[static_cast<std::size_t>(last_tax)].mood_pressure[3] +
            rules().recruit.levels[static_cast<std::size_t>(last_recruit)].mood_pressure[3]));
}

// ---- movement toward the target -------------------------------------------------------------------

TEST_CASE("F8: a month closes a quarter of the gap, truncated, and never jumps over the target") {
    EconomyRules r = EconomyRules::defaults();
    r.mood.base_mood[3] = points(-20);
    MoodInput in = neutral_input();
    in.mood[3] = Fixed32::zero();
    const GroupMood first = compute_group_mood(r, in, Group::Peasants);
    CHECK(first.target == points(-20));
    CHECK(first.mood == points(-5));  // 25 % of the gap of 20

    // from the other side: above the target the mood falls toward it
    in.mood[3] = points(40);
    CHECK(compute_group_mood(r, in, Group::Peasants).mood == points(40 - 15));

    // iterated, it arrives at the target EXACTLY (at least one raw unit a month until it is there)
    Fixed32 mood = Fixed32::zero();
    int months = 0;
    while (mood != points(-20) && months < 200) {
        in.mood[3] = mood;
        const Fixed32 next = compute_group_mood(r, in, Group::Peasants).mood;
        CHECK(next.raw() >= -20 * kOne);  // never past the target
        CHECK(next.raw() <= mood.raw());  // and always on the way
        mood = next;
        ++months;
    }
    CHECK(mood == points(-20));
    CHECK(months < 120);
    // and stays there
    in.mood[3] = mood;
    CHECK(compute_group_mood(r, in, Group::Peasants).mood == mood);
}

TEST_CASE("F8: inertia 0 freezes the mood, 1000 jumps to the target") {
    EconomyRules r = EconomyRules::defaults();
    r.mood.base_mood[3] = points(-20);
    MoodInput in = neutral_input();
    in.mood[3] = points(7);
    r.mood.inertia_permille = 0;
    CHECK(compute_group_mood(r, in, Group::Peasants).mood == points(7));
    r.mood.inertia_permille = 1000;
    CHECK(compute_group_mood(r, in, Group::Peasants).mood == points(-20));
}

TEST_CASE("F8: a mood written out of its range is pulled back into -100..100") {
    EconomyRules r = EconomyRules::defaults();
    r.mood.inertia_permille = 0;
    MoodInput in = neutral_input();
    in.mood[3] = raw(150 * kOne);
    CHECK(compute_group_mood(r, in, Group::Peasants).mood == points(100));
    in.mood[3] = raw(-150 * kOne);
    CHECK(compute_group_mood(r, in, Group::Peasants).mood == points(-100));
}

TEST_CASE("F8: random inputs keep the mood within range and the breakdown equal to the target") {
    Pcg32 rng(81, 1);
    for (int i = 0; i < 3000; ++i) {
        EconomyRules r = EconomyRules::defaults();
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            r.mood.base_mood[g] = raw(rng.range(-60 * Fixed32::one_raw, 60 * Fixed32::one_raw));
        }
        r.mood.wealth_slope_permille = rng.range(0, 3000);
        r.mood.admin_slope_permille = rng.range(0, 3000);
        r.mood.inertia_permille = rng.range(0, 1000);
        MoodInput in;
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            in.mood[g] = raw(rng.range(-120 * Fixed32::one_raw, 120 * Fixed32::one_raw));
            in.wealth[g] = raw(rng.range(0, 300 * Fixed32::one_raw));
            in.events[g] = raw(rng.range(-50 * Fixed32::one_raw, 50 * Fixed32::one_raw));
        }
        in.control = raw(rng.range(0, 100 * Fixed32::one_raw));
        in.tax_level = static_cast<std::uint8_t>(rng.range(0, 6));
        in.recruit_level = static_cast<std::uint8_t>(rng.range(0, 6));
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            const Group group = static_cast<Group>(g);
            Breakdown b;
            const GroupMood with = compute_group_mood(r, in, group, &b);
            const GroupMood without = compute_group_mood(r, in, group);
            REQUIRE(with == without);  // the Breakdown changes nothing
            REQUIRE(b.is_consistent());
            REQUIRE(b.total() == with.target.raw());
            REQUIRE(with.target.raw() >= -100 * kOne);
            REQUIRE(with.target.raw() <= 100 * kOne);
            REQUIRE(with.mood.raw() >= -100 * kOne);
            REQUIRE(with.mood.raw() <= 100 * kOne);
        }
    }
}

// ---- ECO-A1: the consequences of a rate ----------------------------------------------------------

TEST_CASE("ECO-A1: a higher tax rate lowers the mood of the groups that pay it, and only theirs") {
    const MoodInput base = neutral_input();
    std::array<Fixed32, kGroupCount> previous{};
    for (std::uint8_t level = 0; level < rules().tax.level_count; ++level) {
        MoodInput in = base;
        in.tax_level = level;
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            const Fixed32 target = compute_group_target(rules(), in, static_cast<Group>(g));
            if (level > 0) {
                CHECK(target.raw() <= previous[g].raw());  // never better with a heavier rate
            }
            previous[g] = target;
        }
    }
    // level 1 (usual) to level 3 (excessive): -12, -14 and -16 points, the rest does not care
    MoodInput usual = base;
    usual.tax_level = 1;
    MoodInput excessive = base;
    excessive.tax_level = 3;
    const auto drop = [&](Group g) {
        return compute_group_target(rules(), usual, g).raw() -
               compute_group_target(rules(), excessive, g).raw();
    };
    CHECK(drop(Group::Townsfolk) == 12 * kOne);
    CHECK(drop(Group::Peasants) == 14 * kOne);
    CHECK(drop(Group::Cossacks) == 16 * kOne);
    CHECK(drop(Group::Nobility) == 0);
    CHECK(drop(Group::Clergy) == 0);
    CHECK(drop(Group::Natives) == 0);
}

TEST_CASE("ECO-A1: raising the rate of a province in the simulation moves the mood down, month by month") {
    World world = make_world(1);
    Simulation sim(std::move(world));
    install_economy_systems(sim, mood_only_rules());
    // settle at the usual rate first
    for (int month = 0; month < 60; ++month) {
        sim.run_days(
            Date::days_in_month((sim.world().today() + 1).year(), (sim.world().today() + 1).month()));
    }
    const Fixed32 settled = mood_of(sim.world(), 0, Group::Peasants);
    {
        World edited = sim.world();
        edited.economy_columns_mut().tax_policy[0] = 3;
        sim.restore_state(std::move(edited), {});
    }
    Fixed32 previous = settled;
    for (int month = 0; month < 36; ++month) {
        sim.run_days(
            Date::days_in_month((sim.world().today() + 1).year(), (sim.world().today() + 1).month()));
        const Fixed32 now = mood_of(sim.world(), 0, Group::Peasants);
        CHECK(now.raw() <= previous.raw());
        previous = now;
    }
    CHECK(previous.raw() < settled.raw() - 10 * kOne);  // the excessive rate costs well over 10 points
    CHECK(previous.raw() > -100 * kOne);
    require_valid(sim.world());
}

// ---- the shortage of bread reaches the mood only through wealth -------------------------------------

TEST_CASE("F8: the mood never reads the grain: the shortage acts through the wealth term and only there") {
    // two provinces that differ only in their grain
    World world = make_world(2);
    world.economy_columns_mut().grain_stock[0] = Fixed32::zero();
    world.economy_columns_mut().grain_stock[1] = Fixed32::from_int(30'000);
    const MoodInput a = mood_input(world, rules(), ProvinceId(0));
    const MoodInput b = mood_input(world, rules(), ProvinceId(1));
    CHECK(a.wealth == b.wealth);
    CHECK(compute_province_mood(rules(), a) == compute_province_mood(rules(), b));

    // a province that is short of bread has a lower wealth after the production step, and the mood
    // target of the next month falls by exactly the wealth term of that fall, not by one point more
    World hungry = make_world(1);
    const std::size_t row = group_row(0, Group::Peasants);
    const Fixed32 usual_wealth = hungry.economy().groups.wealth[row];
    hungry.economy_columns_mut().wealth[row] = raw(usual_wealth.raw() - 30 * kOne);
    const MoodInput poor = mood_input(hungry, rules(), ProvinceId(0));
    Breakdown rich_b;
    Breakdown poor_b;
    const MoodInput well = mood_input(make_world(1), rules(), ProvinceId(0));
    (void)compute_group_target(rules(), well, Group::Peasants, &rich_b);
    (void)compute_group_target(rules(), poor, Group::Peasants, &poor_b);
    CHECK(poor_b.total() - rich_b.total() == -3 * kOne);  // 30 points of wealth x 0.1
    CHECK(poor_b.find(term::kMoodWealth) != kNoNode);
    // there is no term of the shortage in the family of the mood
    for (std::size_t i = 0; i < poor_b.size(); ++i) {
        const std::uint16_t code = poor_b.node(static_cast<NodeIndex>(i)).code;
        CHECK(code != term::kConsumptionShortage);
        CHECK(code != term::kWealthShortage);
    }
}

// ---- province ---------------------------------------------------------------------------------------

TEST_CASE("province: the step writes the six moods and nothing else") {
    World world = make_world(3);
    world.economy_columns_mut().tax_policy[1] = 3;
    world.economy_columns_mut().recruit_policy[1] = 2;
    const auto before = world.economy_hash_parts();
    const EconomyState untouched = world.economy();

    const ProvinceMood result = step_province_mood(world, rules(), ProvinceId(1));
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        CHECK(world.economy().groups.mood[group_row(1, static_cast<Group>(g))] == result.group[g].mood);
    }
    CHECK(result.group[3].mood.raw() < 0);  // the heavy policies bear on the peasants
    CHECK(result.group[3].target.raw() < 0);
    // the neighbours and every other column are as they were
    CHECK(world.economy().provinces == untouched.provinces);
    CHECK(world.economy().accounts == untouched.accounts);
    CHECK(world.economy().incidents == untouched.incidents);
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        CHECK(world.economy().groups.mood[group_row(0, static_cast<Group>(g))] ==
              untouched.groups.mood[group_row(0, static_cast<Group>(g))]);
        CHECK(world.economy().groups.mood[group_row(2, static_cast<Group>(g))] ==
              untouched.groups.mood[group_row(2, static_cast<Group>(g))]);
    }
    CHECK(world.economy().groups.size == untouched.groups.size);
    CHECK(world.economy().groups.wealth == untouched.groups.wealth);
    CHECK(world.economy_hash_parts() != before);  // the group part moved
    require_valid(world);
}

TEST_CASE("province: a world without economy or an unknown province is left alone") {
    World plain(Date::from_ymd(1702, 12, 31), 5);
    (void)plain.create_dynasty(StringId(1));
    (void)plain.create_province(StringId(100), DynastyId(0), Fixed32::from_int(1));
    CHECK(step_province_mood(plain, rules(), ProvinceId(0)) == ProvinceMood{});
    CHECK(mood_input(plain, rules(), ProvinceId(0)).control == kDefaultControl);

    World world = make_world(1);
    const World copy = world;
    CHECK(step_province_mood(world, rules(), ProvinceId(7)) == ProvinceMood{});
    CHECK(world.compute_hash() == copy.compute_hash());
    apply_province_mood(world, ProvinceId(7), ProvinceMood{});
    CHECK(world.compute_hash() == copy.compute_hash());
}

TEST_CASE("province: explain_mood is the target the step used") {
    World world = make_world(1);
    world.economy_columns_mut().tax_policy[0] = 2;
    const MoodInput in = mood_input(world, rules(), ProvinceId(0));
    const ProvinceMood result = compute_province_mood(rules(), in);
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        const Breakdown b = explain_mood(rules(), in, static_cast<Group>(g));
        REQUIRE(b.is_consistent());
        CHECK(b.total() == result.group[g].target.raw());
    }
}

// ---- time --------------------------------------------------------------------------------------------

TEST_CASE("F8: month lengths of 28, 29, 30 and 31 days, Julian leap years included: one step a month") {
    // 56 provinces, two per slot day; every month each mood has moved by exactly one step toward the
    // target, whatever the length of the month (1704, 1708 and 1800 are Julian leap years)
    EconomyRules r = mood_only_rules();
    r.mood.inertia_permille = 400;
    for (const int start_year : {1703, 1707, 1799}) {
        constexpr std::uint32_t kProvinces = 56;
        World world = make_world(kProvinces, 1000, Date::from_ymd(start_year, 12, 31));
        std::array<Fixed32, kProvinces> expected{};
        for (std::uint32_t p = 0; p < kProvinces; ++p) {
            world.economy_columns_mut().tax_policy[p] = 3;
            expected[p] = mood_of(world, p, Group::Peasants);
        }
        const MoodInput in = mood_input(world, rules(), ProvinceId(0));
        Simulation sim(std::move(world));
        install_economy_systems(sim, r);
        bool saw_february_of_29 = false;
        bool saw_month_of_30 = false;
        for (int month = 1; month <= 12; ++month) {
            const Date first = sim.world().today() + 1;
            const int days = Date::days_in_month(first.year(), first.month());
            saw_february_of_29 = saw_february_of_29 || (first.month() == 2 && days == 29);
            saw_month_of_30 = saw_month_of_30 || days == 30;
            sim.run_days(days);
            for (std::uint32_t p = 0; p < kProvinces; ++p) {
                // all provinces are alike (only the stagger differs), so the same step is expected
                MoodInput step = in;
                step.mood[3] = expected[p];
                expected[p] = compute_group_mood(r, step, Group::Peasants).mood;
                REQUIRE(mood_of(sim.world(), p, Group::Peasants) == expected[p]);
            }
        }
        CHECK(saw_february_of_29);
        CHECK(saw_month_of_30);
        require_valid(sim.world());
    }
}
