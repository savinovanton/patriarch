#include "pat/core/rng.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/economy/administrator.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/mood.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/economy/unrest.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

// E9-T11: the unrest of a province and one-off riots (docs/dev/economy-model.md section 9 formulas F9 and
// F10, section 7.3 steps 5 and 7, section 7.4; GDD 03 section 4.3): the states and their limits, the
// weighted average and its Breakdown, the chance and the single draw, the loot, the fall of control, the
// incident log, the uprising flag, and fifty years in which unrest follows the policies and sticks nowhere.

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

void require_valid(const World& world) {
    const auto problems = world.check_invariants();
    for (const auto& problem : problems) {
        MESSAGE(problem);
    }
    REQUIRE(problems.empty());
}

/// A province with the people of a small estate: 30 nobles, 20 clergy, 150 townsfolk, 700 peasants and
/// 100 cossacks, bread for all, the usual policies, dynasty 0 as the owner.
World make_world(std::uint32_t provinces, Date start = Date::from_ymd(1702, 12, 31)) {
    World world(start, 5);
    (void)world.create_dynasty(StringId(1));
    (void)world.create_dynasty(StringId(2));
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.create_province(StringId(100 + p), DynastyId(0), Fixed32::from_int(1));
    }
    REQUIRE(world.enable_economy());
    const std::array<std::uint32_t, kGroupCount> sizes{30, 20, 150, 700, 100, 0};
    for (std::uint32_t p = 0; p < provinces; ++p) {
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            (void)world.set_group_size(ProvinceId(p), static_cast<Group>(g), sizes[g]);
        }
        world.economy_columns_mut().capacity[p * kMaxGoods] = Fixed32::from_int(30'000);  // grain
        world.economy_columns_mut().tax_policy[p] = static_cast<std::uint8_t>(rules().tax.default_level);
        world.economy_columns_mut().recruit_policy[p] =
            static_cast<std::uint8_t>(rules().recruit.default_level);
    }
    require_valid(world);
    return world;
}

void run_months(Simulation& sim, int months) {
    for (int i = 0; i < months; ++i) {
        const Date next = sim.world().today() + 1;
        sim.run_days(Date::days_in_month(next.year(), next.month()));
    }
}

std::size_t incidents_of(const World& world, IncidentKind kind) {
    std::size_t count = 0;
    for (const std::uint8_t k : world.economy().incidents.kind) {
        count += k == static_cast<std::uint8_t>(kind) ? 1 : 0;
    }
    return count;
}

/// The unrest of a province with one group of `size` people at `mood` and everything else neutral.
UnrestInput one_group(Group group, std::uint32_t size, Fixed32 mood) {
    UnrestInput in;
    in.size[index_of(group)] = size;
    in.mood[index_of(group)] = mood;
    in.control = points(70);
    return in;
}

/// Changes the world of a running simulation (there is no command for it yet): copy, edit, put back.
template <class Edit>
void edit(Simulation& sim, Edit&& change) {
    World world = sim.world();
    change(world);
    sim.restore_state(std::move(world), {});
}

Kopecks treasury(const World& world, std::size_t row) {
    return world.economy().accounts.treasury(row);
}

}  // namespace

// ---- the states -----------------------------------------------------------------------------------

TEST_CASE("unrest state: the limits 25, 50 and 75 are inclusive below and exclusive above") {
    const UnrestRules& u = rules().unrest;
    REQUIRE(u.calm_below == 25);
    REQUIRE(u.riot_from == 50);
    REQUIRE(u.uprising_from == 75);
    const auto state = [&](std::int64_t value_raw) { return unrest_state(u, raw(value_raw)); };
    CHECK(state(0) == UnrestState::Calm);
    CHECK(state(25 * kOne - 1) == UnrestState::Calm);  // 24.99998
    CHECK(state(25 * kOne) == UnrestState::Agitation);
    CHECK(state(50 * kOne - 1) == UnrestState::Agitation);
    CHECK(state(50 * kOne) == UnrestState::Riot);
    CHECK(state(75 * kOne - 1) == UnrestState::Riot);
    CHECK(state(75 * kOne) == UnrestState::Uprising);
    CHECK(state(100 * kOne) == UnrestState::Uprising);
    CHECK_FALSE(is_uprising(u, raw(75 * kOne - 1)));
    CHECK(is_uprising(u, raw(75 * kOne)));
}

TEST_CASE("unrest state: the limits come from the rules") {
    UnrestRules u = rules().unrest;
    u.calm_below = 10;
    u.riot_from = 20;
    u.uprising_from = 30;
    CHECK(unrest_state(u, points(9)) == UnrestState::Calm);
    CHECK(unrest_state(u, points(10)) == UnrestState::Agitation);
    CHECK(unrest_state(u, points(20)) == UnrestState::Riot);
    CHECK(unrest_state(u, points(30)) == UnrestState::Uprising);
}

// ---- F9 -------------------------------------------------------------------------------------------

TEST_CASE("F9: only a sullen group adds unrest, in proportion to how sullen it is") {
    const UnrestRules& u = rules().unrest;
    REQUIRE(u.slope_permille == 4000);
    CHECK(group_unrest(u, points(5)) == Fixed32::zero());  // content: nothing
    CHECK(group_unrest(u, Fixed32::zero()) == Fixed32::zero());
    CHECK(group_unrest(u, points(-5)) == points(20));
    CHECK(group_unrest(u, raw(-3 * kOne / 2)) == points(6));
    CHECK(group_unrest(u, points(-30)) == points(100));  // clamped
    CHECK(group_unrest(u, points(-100)) == points(100));
}

TEST_CASE("F9 target: one group, hand-computed, with the breakdown of every term") {
    // 700 peasants at mood -5: 20 points; control 30 is 10 under the low threshold: 5 points; events 2
    UnrestInput in = one_group(Group::Peasants, 700, points(-5));
    in.control = points(30);
    in.events = points(2);
    Breakdown b;
    const Fixed32 target = compute_unrest_target(rules(), in, &b);
    CHECK(target == points(27));
    REQUIRE(b.is_consistent());
    CHECK(b.root().code == term::kUnrestTarget);
    CHECK(b.root().unit == Unit::Points);
    CHECK(b.total() == target.raw());
    CHECK(b.node(b.find(term::kUnrestGroupFirst + 3)).value == 20 * kOne);
    CHECK(b.node(b.find(term::kUnrestControl)).value == 5 * kOne);
    CHECK(b.node(b.find(term::kUnrestControl)).factor_permille == rules().unrest.control_slope_permille);
    CHECK(b.node(b.find(term::kUnrestEvents)).value == 2 * kOne);
    // the groups that add nothing, and the clamp that did not act, are not listed
    CHECK(b.find(term::kUnrestGroupFirst + 2) == kNoNode);
    CHECK(b.find(term::kUnrestLimit) == kNoNode);
    CHECK(b.size() == 4);
}

TEST_CASE("F9 target: the weights are size x influence, hand-computed over three groups") {
    // townsfolk: 100 x 200 = 20 000 at mood -10 -> u = 40; peasants: 600 x 100 = 60 000 at mood -2.5 -> 10;
    // cossacks: 80 x 250 = 20 000 at +3 -> 0. Average = (20000 x 40 + 60000 x 10) / 100000 = 14.
    UnrestInput in;
    in.size[2] = 100;
    in.mood[2] = points(-10);
    in.size[3] = 600;
    in.mood[3] = raw(-5 * kOne / 2);
    in.size[4] = 80;
    in.mood[4] = points(3);
    in.control = points(100);
    Breakdown b;
    CHECK(compute_unrest_target(rules(), in, &b) == points(14));
    REQUIRE(b.is_consistent());
    CHECK(b.node(b.find(term::kUnrestGroupFirst + 2)).value == 8 * kOne);  // 0.2 x 40
    CHECK(b.node(b.find(term::kUnrestGroupFirst + 3)).value == 6 * kOne);  // 0.6 x 10
    CHECK(b.find(term::kUnrestGroupFirst + 4) == kNoNode);
}

TEST_CASE("F9 target: the contributions of the groups add up to the average exactly, truncation included") {
    // sizes and moods that do not divide evenly: the cumulative split loses not one raw unit
    Pcg32 rng(55, 2);
    for (int i = 0; i < 3000; ++i) {
        UnrestInput in;
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            in.size[g] = static_cast<std::uint32_t>(rng.range(0, 90'000));
            in.mood[g] = raw(rng.range(-60 * Fixed32::one_raw, 20 * Fixed32::one_raw));
        }
        in.control = raw(rng.range(0, 100 * Fixed32::one_raw));
        in.events = raw(rng.range(-10 * Fixed32::one_raw, 10 * Fixed32::one_raw));
        Breakdown b;
        const Fixed32 with = compute_unrest_target(rules(), in, &b);
        REQUIRE(with == compute_unrest_target(rules(), in));  // the Breakdown changes nothing
        REQUIRE(b.is_consistent());
        REQUIRE(b.total() == with.raw());
        REQUIRE(with.raw() >= 0);
        REQUIRE(with.raw() <= 100 * kOne);
        for (std::size_t n = 1; n < b.size(); ++n) {
            const std::uint16_t code = b.node(static_cast<NodeIndex>(n)).code;
            if (code >= term::kUnrestGroupFirst && code < term::kUnrestGroupFirst + kGroupCount) {
                REQUIRE(b.node(static_cast<NodeIndex>(n)).value > 0);  // a group is listed only if it counts
            }
        }
    }
}

TEST_CASE("F9 target: a province without people has only control and events to go on") {
    UnrestInput in;
    in.mood.fill(points(-50));
    in.control = points(100);
    CHECK(compute_unrest_target(rules(), in) == Fixed32::zero());
    in.control = points(20);  // 20 under the threshold: 10 points
    in.events = points(1);
    Breakdown b;
    CHECK(compute_unrest_target(rules(), in, &b) == points(11));
    REQUIRE(b.is_consistent());
    CHECK(b.size() == 3);
}

TEST_CASE("F9 target: the control term acts below the low threshold only, and is exact at its edge") {
    UnrestInput in = one_group(Group::Peasants, 100, points(10));  // content
    in.control = points(40);                                       // exactly the threshold: nothing
    CHECK(compute_unrest_target(rules(), in) == Fixed32::zero());
    in.control = raw(40 * kOne - 2);  // two raw units under it: 0.5 x 2 raw = 1 raw
    CHECK(compute_unrest_target(rules(), in).raw() == 1);
    in.control = points(0);  // 40 under: 20 points
    CHECK(compute_unrest_target(rules(), in) == points(20));
    in.control = points(100);
    CHECK(compute_unrest_target(rules(), in) == Fixed32::zero());
}

TEST_CASE("F9 target: the clamp to 0..100 is a term of its own") {
    UnrestInput in = one_group(Group::Cossacks, 500, points(-90));
    in.control = points(0);
    in.events = points(30);
    Breakdown b;
    CHECK(compute_unrest_target(rules(), in, &b) == points(100));
    REQUIRE(b.is_consistent());
    // 100 + 20 + 30 = 150: the clamp takes 50 points away
    CHECK(b.node(b.find(term::kUnrestLimit)).value == -50 * kOne);

    in = one_group(Group::Cossacks, 500, points(10));
    in.control = points(100);
    in.events = points(-12);
    Breakdown below;
    CHECK(compute_unrest_target(rules(), in, &below) == Fixed32::zero());
    REQUIRE(below.is_consistent());
    CHECK(below.node(below.find(term::kUnrestLimit)).value == 12 * kOne);
}

TEST_CASE("F9 target: the largest legal province does not overflow") {
    UnrestInput in;
    in.size.fill(kMaxProvincePersons);  // more than a legal province: the function must still not wrap
    in.mood.fill(points(-100));
    in.control = points(0);
    EconomyRules r = EconomyRules::defaults();
    r.unrest.influence_permille.fill(kMaxMultiplierPermille);
    Breakdown b;
    CHECK(compute_unrest_target(r, in, &b) == points(100));
    REQUIRE(b.is_consistent());
    in.size = {kMaxProvincePersons, 0, 0, 0, 0, 0};
    CHECK(compute_unrest_target(r, in) == points(100));
    in.size.fill(std::numeric_limits<std::uint32_t>::max());  // above the cap: counted as the cap
    CHECK(compute_unrest_target(r, in) == points(100));
}

TEST_CASE("F9: a month closes a quarter of the gap, never jumps over the target, arrives exactly") {
    UnrestInput in = one_group(Group::Peasants, 700, points(-10));  // target 40
    in.unrest = Fixed32::zero();
    UnrestResult r = compute_unrest(rules(), in);
    CHECK(r.target == points(40));
    CHECK(r.unrest == points(10));
    CHECK(r.before == UnrestState::Calm);
    CHECK(r.after == UnrestState::Calm);
    // from above
    in.unrest = points(80);
    r = compute_unrest(rules(), in);
    CHECK(r.unrest == points(70));
    CHECK(r.before == UnrestState::Uprising);
    CHECK(r.after == UnrestState::Riot);

    Fixed32 value = Fixed32::zero();
    int months = 0;
    while (value != points(40) && months < 300) {
        in.unrest = value;
        const Fixed32 next = compute_unrest(rules(), in).unrest;
        REQUIRE(next.raw() <= 40 * kOne);
        REQUIRE(next.raw() >= value.raw());
        value = next;
        ++months;
    }
    CHECK(value == points(40));
    in.unrest = value;
    CHECK(compute_unrest(rules(), in).unrest == value);  // and stays there
}

TEST_CASE("F9: inertia 0 freezes unrest, a column out of its range is pulled back, the state is classified") {
    EconomyRules r = EconomyRules::defaults();
    r.unrest.inertia_permille = 0;
    UnrestInput in = one_group(Group::Peasants, 700, points(-10));
    in.unrest = points(33);
    CHECK(compute_unrest(r, in).unrest == points(33));
    in.unrest = raw(130 * kOne);
    const UnrestResult clamped = compute_unrest(r, in);
    CHECK(clamped.unrest == points(100));
    CHECK(clamped.before == UnrestState::Uprising);
    in.unrest = raw(-5 * kOne);
    CHECK(compute_unrest(r, in).unrest == Fixed32::zero());
}

// ---- F10 ------------------------------------------------------------------------------------------

TEST_CASE("F10: the chance is 0 below the threshold, the minimum at it, the maximum at 100, linear between") {
    const UnrestRules& u = rules().unrest;
    CHECK(riot_chance_permille(u, Fixed32::zero()) == 0);
    CHECK(riot_chance_permille(u, raw(50 * kOne - 1)) == 0);
    CHECK(riot_chance_permille(u, points(50)) == 20);
    CHECK(riot_chance_permille(u, points(75)) == 135);  // 20 + 230 / 2
    CHECK(riot_chance_permille(u, points(100)) == 250);
    CHECK(riot_chance_permille(u, raw(120 * kOne)) == 250);  // a column out of its range
    // truncated toward zero: 20 + 230 x 1 / 50 = 24.6 -> 24
    CHECK(riot_chance_permille(u, points(51)) == 24);
    // monotonic over the whole range
    std::int32_t previous = 0;
    for (std::int64_t v = 0; v <= 100 * kOne; v += 977) {
        const std::int32_t chance = riot_chance_permille(u, raw(v));
        REQUIRE(chance >= previous);
        previous = chance;
    }
}

TEST_CASE("F10: the chance follows the rules") {
    UnrestRules u = rules().unrest;
    u.riot_from = 60;
    u.riot_chance_min_permille = 100;
    u.riot_chance_max_permille = 500;
    CHECK(riot_chance_permille(u, points(59)) == 0);
    CHECK(riot_chance_permille(u, points(60)) == 100);
    CHECK(riot_chance_permille(u, points(80)) == 300);
    CHECK(riot_chance_permille(u, points(100)) == 500);
}

TEST_CASE("F10: the draw is exactly one call of bounded(1000), whatever the chance") {
    for (const std::int32_t chance : {0, 1, 250, 999, 1000}) {
        Pcg32 used(9, 4);
        Pcg32 reference(9, 4);
        const bool result = roll_riot(used, chance);
        const std::uint32_t number = reference.bounded(1000);
        CHECK(used.state() == reference.state());
        CHECK(result == (static_cast<std::int32_t>(number) < chance));
    }
    // the frequency is the chance (a loose check on 20 000 draws)
    Pcg32 rng(123, 1);
    int hits = 0;
    for (int i = 0; i < 20'000; ++i) {
        hits += roll_riot(rng, 250) ? 1 : 0;
    }
    CHECK(hits > 4'600);
    CHECK(hits < 5'400);
}

TEST_CASE("F10: the loot is a share of the income, the loss of control is not below zero") {
    UnrestRules u = rules().unrest;
    REQUIRE(u.riot_loot_permille == 300);
    REQUIRE(u.riot_control_loss == 5);
    CHECK(riot_loot(u, 10'000) == 3'000);
    CHECK(riot_loot(u, 1) == 0);  // truncated toward zero
    CHECK(riot_loot(u, 7) == 2);
    CHECK(riot_loot(u, 0) == 0);
    CHECK(riot_loot(u, -500) == 0);
    CHECK(riot_loot(u, std::numeric_limits<Kopecks>::max()) <= kMaxPostingKop);
    CHECK(control_after_riot(u, points(70)) == points(65));
    CHECK(control_after_riot(u, points(3)) == Fixed32::zero());
    CHECK(control_after_riot(u, Fixed32::zero()) == Fixed32::zero());
    u.riot_control_loss = 0;
    CHECK(control_after_riot(u, points(70)) == points(70));
}

// ---- province: the steps -----------------------------------------------------------------------------

TEST_CASE("province: step_province_unrest writes the column and the incident log, not the control") {
    World world = make_world(2);
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        world.economy_columns_mut().mood[group_row(1, static_cast<Group>(g))] = points(-12);
    }
    world.economy_columns_mut().unrest[1] = points(24);
    const UnrestInput in = unrest_input(world, ProvinceId(1));
    CHECK(in.unrest == points(24));
    CHECK(in.size[3] == 700);
    CHECK(in.mood[3] == points(-12));
    const UnrestResult expected = compute_unrest(rules(), in);
    const EconomyState before = world.economy();

    const UnrestResult got = step_province_unrest(world, rules(), ProvinceId(1));
    CHECK(got == expected);
    CHECK(world.economy().provinces.unrest[1] == expected.unrest);
    CHECK(world.economy().provinces.unrest[0] == before.provinces.unrest[0]);
    CHECK(world.economy().provinces.control == before.provinces.control);
    CHECK(world.economy().groups == before.groups);
    CHECK(world.economy().accounts == before.accounts);
    // 24 is calm and the step took it into agitation: one entry, the new state in `param`
    REQUIRE(expected.before == UnrestState::Calm);
    REQUIRE(expected.after == UnrestState::Agitation);
    REQUIRE(world.economy().incidents.size() == 1);
    CHECK(world.economy().incidents.kind[0] == static_cast<std::uint8_t>(IncidentKind::UnrestStateChanged));
    CHECK(world.economy().incidents.province[0] == ProvinceId(1));
    CHECK(world.economy().incidents.dynasty[0] == DynastyId(0));
    CHECK(world.economy().incidents.param[0] == static_cast<std::int32_t>(UnrestState::Agitation));
    require_valid(world);
}

TEST_CASE("province: a province without economy or an unknown one is left alone by the steps") {
    World plain(Date::from_ymd(1702, 12, 31), 5);
    (void)plain.create_dynasty(StringId(1));
    (void)plain.create_province(StringId(100), DynastyId(0), Fixed32::from_int(1));
    CHECK(step_province_unrest(plain, rules(), ProvinceId(0)) == UnrestResult{});
    CHECK(step_province_riot(plain, rules(), ProvinceId(0)) == RiotOutcome{});
    CHECK(plain.rng().size() == 0);

    World world = make_world(1);
    world.economy_columns_mut().unrest[0] = points(90);
    const World copy = world;
    CHECK(step_province_unrest(world, rules(), ProvinceId(9)) == UnrestResult{});
    CHECK(step_province_riot(world, rules(), ProvinceId(9)) == RiotOutcome{});
    CHECK(world.compute_hash() == copy.compute_hash());
}

TEST_CASE("province: a state change is written once, the uprising on entering it and not again") {
    EconomyRules r = rules();
    r.unrest.inertia_permille = 1000;  // the target is reached in one step: states are easy to steer
    World world = make_world(1);
    const auto steer = [&](std::int64_t mood_points) {
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            world.economy_columns_mut().mood[group_row(0, static_cast<Group>(g))] = points(mood_points);
        }
        (void)step_province_unrest(world, r, ProvinceId(0));
    };
    // all six groups at the same mood m: target = 4 x -m (the groups with people dominate the weights)
    steer(0);  // calm stays calm: no entry
    CHECK(world.economy().incidents.size() == 0);
    steer(-8);  // 32: agitation
    CHECK(world.economy().provinces.unrest[0] == points(32));
    steer(-9);  // 36: still agitation, no entry
    CHECK(world.economy().incidents.size() == 1);
    steer(-14);  // 56: riot
    steer(-20);  // 80: uprising
    CHECK(is_uprising(r.unrest, world.economy().provinces.unrest[0]));
    CHECK(incidents_of(world, IncidentKind::Uprising) == 1);
    CHECK(incidents_of(world, IncidentKind::UnrestStateChanged) == 3);  // agitation, riot, uprising
    steer(-22);                                                         // 88: still the uprising: nothing new
    CHECK(incidents_of(world, IncidentKind::Uprising) == 1);
    CHECK(incidents_of(world, IncidentKind::UnrestStateChanged) == 3);
    steer(-14);  // 56: out of the uprising into the riot
    CHECK(incidents_of(world, IncidentKind::Uprising) == 1);
    steer(-20);  // 80: in again: a second uprising
    CHECK(incidents_of(world, IncidentKind::Uprising) == 2);
    steer(5);  // calm
    // the uprising entry names the unrest in whole points
    std::vector<std::int32_t> uprising_params;
    for (std::size_t i = 0; i < world.economy().incidents.size(); ++i) {
        if (world.economy().incidents.kind[i] == static_cast<std::uint8_t>(IncidentKind::Uprising)) {
            uprising_params.push_back(world.economy().incidents.param[i]);
        }
    }
    CHECK(uprising_params == std::vector<std::int32_t>{80, 80});
    // the states recorded by the changes follow the path of the steering
    std::vector<std::int32_t> path;
    for (std::size_t i = 0; i < world.economy().incidents.size(); ++i) {
        if (world.economy().incidents.kind[i] ==
            static_cast<std::uint8_t>(IncidentKind::UnrestStateChanged)) {
            path.push_back(world.economy().incidents.param[i]);
        }
    }
    CHECK(path == std::vector<std::int32_t>{1, 2, 3, 2, 3, 0});
    require_valid(world);
}

TEST_CASE("province: the entries about a crown or an ownerless province name no dynasty") {
    EconomyRules r = rules();
    r.unrest.inertia_permille = 1000;
    World world = make_world(2);
    REQUIRE(world.set_province_owner_kind(ProvinceId(0), OwnerKind::Crown));
    REQUIRE(world.set_province_owner_kind(ProvinceId(1), OwnerKind::Unowned));
    for (std::uint32_t p = 0; p < 2; ++p) {
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            world.economy_columns_mut().mood[group_row(p, static_cast<Group>(g))] = points(-10);
        }
        (void)step_province_unrest(world, r, ProvinceId(p));
    }
    REQUIRE(world.economy().incidents.size() == 2);
    CHECK(world.economy().incidents.dynasty[0] == DynastyId());
    CHECK(world.economy().incidents.dynasty[1] == DynastyId());
    CHECK(world.economy().incidents.province[0] == ProvinceId(0));
    CHECK(world.economy().incidents.province[1] == ProvinceId(1));
}

TEST_CASE("riot: below the threshold nothing is drawn, at it exactly one number is") {
    World world = make_world(1);
    world.economy_columns_mut().unrest[0] = raw(50 * kOne - 1);
    const RiotOutcome none = step_province_riot(world, rules(), ProvinceId(0));
    CHECK_FALSE(none.rolled);
    CHECK_FALSE(none.happened);
    CHECK(world.rng().size() == 0);  // the stream was not even created

    world.economy_columns_mut().unrest[0] = points(50);
    const RiotOutcome rolled = step_province_riot(world, rules(), ProvinceId(0));
    CHECK(rolled.rolled);
    CHECK(rolled.chance_permille == 20);
    CHECK(world.rng().size() == 1);
    // the stream is the one the spec names, advanced by exactly one call of bounded(1000)
    World other(Date::from_ymd(1702, 12, 31), 5);
    Pcg32& reference = other.rng().stream("economy.unrest");
    const bool first = roll_riot(reference, 20);
    CHECK(rolled.happened == first);
    CHECK(world.rng().stream("economy.unrest").state() == reference.state());
}

TEST_CASE("riot: a success takes a share of the income from the owner, lowers control, writes the entry") {
    EconomyRules r = rules();
    r.unrest.riot_chance_min_permille = 1000;  // sure
    r.unrest.riot_chance_max_permille = 1000;
    World world = make_world(1);
    world.economy_columns_mut().unrest[0] = points(80);
    world.economy_columns_mut().control[0] = points(70);
    world.economy_columns_mut().income_last_month[0] = 10'000;
    const Kopecks owner_before = treasury(world, 1);
    const Kopecks crown_before = treasury(world, 0);

    const RiotOutcome out = step_province_riot(world, r, ProvinceId(0));
    CHECK(out.rolled);
    CHECK(out.happened);
    CHECK(out.chance_permille == 1000);
    CHECK(out.loot_kop == 3'000);
    CHECK(out.refused_kop == 0);
    CHECK(out.control == points(65));
    CHECK(world.economy().provinces.control[0] == points(65));
    CHECK(treasury(world, 1) == owner_before - 3'000);
    CHECK(treasury(world, 0) == crown_before);
    CHECK(world.economy().accounts.open_line(1, LedgerCode::RaidLoss) == -3'000);
    // the base of the next riot is untouched
    CHECK(world.economy().provinces.income_last_month[0] == 10'000);
    // the entry: province, the owner's dynasty, the loss in kopecks
    REQUIRE(world.economy().incidents.size() == 1);
    CHECK(world.economy().incidents.kind[0] == static_cast<std::uint8_t>(IncidentKind::Riot));
    CHECK(world.economy().incidents.province[0] == ProvinceId(0));
    CHECK(world.economy().incidents.dynasty[0] == DynastyId(0));
    CHECK(world.economy().incidents.param[0] == 3'000);
    require_valid(world);  // the books still balance: E-M1 and the pairs of E-M3
}

TEST_CASE("riot: no chance, no riot; the draw is made all the same") {
    EconomyRules r = rules();
    r.unrest.riot_chance_min_permille = 0;
    r.unrest.riot_chance_max_permille = 0;
    World world = make_world(1);
    world.economy_columns_mut().unrest[0] = points(90);
    world.economy_columns_mut().income_last_month[0] = 10'000;
    const EconomyState before = world.economy();
    const RiotOutcome out = step_province_riot(world, r, ProvinceId(0));
    CHECK(out.rolled);
    CHECK_FALSE(out.happened);
    CHECK(out.chance_permille == 0);
    CHECK(out.control == before.provinces.control[0]);
    CHECK(world.economy() == before);  // not a kopeck, not a point, not an entry
    CHECK(world.rng().size() == 1);
}

TEST_CASE("riot: the crown's province pays the crown, an ownerless one costs nobody anything") {
    EconomyRules r = rules();
    r.unrest.riot_chance_min_permille = 1000;
    r.unrest.riot_chance_max_permille = 1000;
    World world = make_world(2);
    REQUIRE(world.set_province_owner_kind(ProvinceId(0), OwnerKind::Crown));
    REQUIRE(world.set_province_owner_kind(ProvinceId(1), OwnerKind::Unowned));
    for (std::uint32_t p = 0; p < 2; ++p) {
        world.economy_columns_mut().unrest[p] = points(60);
        world.economy_columns_mut().income_last_month[p] = 1'000;
    }
    const Kopecks crown_before = treasury(world, 0);
    const Kopecks owner_before = treasury(world, 1);

    CHECK(step_province_riot(world, r, ProvinceId(0)).loot_kop == 300);
    CHECK(treasury(world, 0) == crown_before - 300);
    CHECK(treasury(world, 1) == owner_before);
    const RiotOutcome ownerless = step_province_riot(world, r, ProvinceId(1));
    CHECK(ownerless.happened);
    CHECK(ownerless.loot_kop == 0);                             // nobody to rob
    CHECK(world.economy().provinces.control[1] == points(95));  // but the grip is lost all the same
    CHECK(treasury(world, 0) == crown_before - 300);
    CHECK(treasury(world, 1) == owner_before);
    REQUIRE(world.economy().incidents.size() == 2);
    CHECK(world.economy().incidents.dynasty[0] == DynastyId());
    CHECK(world.economy().incidents.dynasty[1] == DynastyId());
    CHECK(world.economy().incidents.param[1] == 0);
    require_valid(world);
}

TEST_CASE("riot: a province with no income loses no money but still a riot is a riot") {
    EconomyRules r = rules();
    r.unrest.riot_chance_min_permille = 1000;
    r.unrest.riot_chance_max_permille = 1000;
    World world = make_world(1);
    world.economy_columns_mut().unrest[0] = points(55);
    world.economy_columns_mut().income_last_month[0] = 0;
    const Kopecks owner_before = treasury(world, 1);
    const RiotOutcome out = step_province_riot(world, r, ProvinceId(0));
    CHECK(out.happened);
    CHECK(out.loot_kop == 0);
    CHECK(treasury(world, 1) == owner_before);
    CHECK(world.economy().provinces.control[0] == points(95));
    CHECK(incidents_of(world, IncidentKind::Riot) == 1);
}

TEST_CASE("riot: the loot is posted through the books, and a full account refuses what it cannot take") {
    EconomyRules r = rules();
    r.unrest.riot_chance_min_permille = 1000;
    r.unrest.riot_chance_max_permille = 1000;
    r.unrest.riot_loot_permille = 1000;
    World world = make_world(1);
    world.economy_columns_mut().unrest[0] = points(60);
    world.economy_columns_mut().income_last_month[0] = 500;
    // an account that is already at the bottom of its range refuses the whole loot
    for (Kopecks drained = 0; drained <= kMaxTreasuryKop; drained += kMaxPostingKop) {
        (void)world.post_money(AccountRef::of(DynastyId(0)), LedgerCode::Adjustment, -kMaxPostingKop);
    }
    REQUIRE(treasury(world, 1) == -kMaxTreasuryKop);
    const RiotOutcome out = step_province_riot(world, r, ProvinceId(0));
    CHECK(out.happened);
    CHECK(out.loot_kop == 0);
    CHECK(out.refused_kop == 500);
    CHECK(treasury(world, 1) == -kMaxTreasuryKop);
    require_valid(world);
}

// ---- in time: the system ------------------------------------------------------------------------------

TEST_CASE("unrest: raising the rate raises the unrest, lowering it brings it back (ECO-A1)") {
    World world = make_world(1);
    Simulation sim(std::move(world));
    EconomyRules r = rules();
    r.unrest.riot_chance_min_permille = 0;  // no riots: this test is about the rate
    r.unrest.riot_chance_max_permille = 0;
    install_economy_systems(sim, r);
    run_months(sim, 96);
    const Fixed32 usual_unrest = sim.world().economy().provinces.unrest[0];
    const Fixed32 usual_mood = sim.world().economy().groups.mood[group_row(0, Group::Peasants)];
    CHECK(unrest_state(r.unrest, usual_unrest) == UnrestState::Calm);

    edit(sim, [](World& w) { w.economy_columns_mut().tax_policy[0] = 3; });  // excessive
    Fixed32 previous_unrest = usual_unrest;
    for (int month = 0; month < 48; ++month) {
        run_months(sim, 1);
        CHECK(sim.world().economy().provinces.unrest[0].raw() >= previous_unrest.raw());  // only up
        previous_unrest = sim.world().economy().provinces.unrest[0];
    }
    const Fixed32 hot_mood = sim.world().economy().groups.mood[group_row(0, Group::Peasants)];
    CHECK(hot_mood.raw() < usual_mood.raw() - 10 * kOne);
    CHECK(previous_unrest.raw() > usual_unrest.raw() + 40 * kOne);
    CHECK(previous_unrest.raw() < 100 * kOne);
    CHECK(unrest_state(r.unrest, previous_unrest) >= UnrestState::Riot);  // the GDD: "N months, then 50"

    edit(sim, [](World& w) { w.economy_columns_mut().tax_policy[0] = 1; });  // back to the usual
    run_months(sim, 96);
    CHECK(unrest_state(r.unrest, sim.world().economy().provinces.unrest[0]) == UnrestState::Calm);
    CHECK(sim.world().economy().provinces.unrest[0].raw() <= usual_unrest.raw() + kOne);
    require_valid(sim.world());
}

TEST_CASE("unrest: fifty years under the usual policies stay calm; under heavy ones follow the policy, never "
          "stick") {
    struct Scenario {
        std::uint8_t tax;
        std::uint8_t recruit;
        UnrestState lowest;
        UnrestState highest;
        std::int64_t least_control;  ///< the lowest control any province may touch in the last ten years
    };
    // the unrest tracks the policy: low and usual taxes are calm, high is agitation, excessive is riot
    const std::array<Scenario, 5> scenarios{{
        {0, 0, UnrestState::Calm, UnrestState::Calm, 60},
        {1, 1, UnrestState::Calm, UnrestState::Calm, 60},
        {2, 1, UnrestState::Agitation, UnrestState::Agitation, 55},
        {3, 1, UnrestState::Riot, UnrestState::Riot, 30},
        {3, 2, UnrestState::Riot, UnrestState::Uprising, 20},
    }};
    for (const Scenario& scenario : scenarios) {
        World world = make_world(8);
        for (std::uint32_t p = 0; p < 8; ++p) {
            world.economy_columns_mut().tax_policy[p] = scenario.tax;
            world.economy_columns_mut().recruit_policy[p] = scenario.recruit;
        }
        Simulation sim(std::move(world));
        install_economy_systems(sim, rules());
        std::int64_t peak = 0;
        std::int64_t tail_low = 100 * kOne;
        std::int64_t tail_high = 0;
        std::int64_t control_low = 100 * kOne;
        for (int month = 0; month < 600; ++month) {
            run_months(sim, 1);
            if (month % 12 == 11) {
                require_valid(sim.world());
            }
            for (std::uint32_t p = 0; p < 8; ++p) {
                const std::int64_t u = sim.world().economy().provinces.unrest[p].raw();
                peak = std::max(peak, u);
                if (month >= 480) {  // the last ten years
                    tail_low = std::min(tail_low, u);
                    tail_high = std::max(tail_high, u);
                    control_low =
                        std::min<std::int64_t>(control_low, sim.world().economy().provinces.control[p].raw());
                }
            }
        }
        INFO("tax " << static_cast<int>(scenario.tax) << " recruit " << static_cast<int>(scenario.recruit));
        const UnrestRules& u = rules().unrest;
        CHECK(unrest_state(u, raw(tail_low)) >= scenario.lowest);
        CHECK(unrest_state(u, raw(tail_high)) <= scenario.highest);
        CHECK(peak < 100 * kOne);  // never glued to the ceiling
        CHECK(tail_high - tail_low < 40 * kOne);
        // control stays in the working range: the grip is lost in a riot and regained between riots
        CHECK(control_low > scenario.least_control * kOne);
    }
}

TEST_CASE("unrest: from the extremes the province finds its level, it does not stay at 0 or at 100") {
    for (const std::int64_t start : {std::int64_t{0}, std::int64_t{100}}) {
        for (const std::uint8_t tax : {std::uint8_t{1}, std::uint8_t{3}}) {
            World world = make_world(1);
            world.economy_columns_mut().unrest[0] = raw(start * kOne);
            world.economy_columns_mut().tax_policy[0] = tax;
            Simulation sim(std::move(world));
            install_economy_systems(sim, rules());
            run_months(sim, 360);
            const Fixed32 settled = sim.world().economy().provinces.unrest[0];
            INFO("start " << start << " tax " << static_cast<int>(tax));
            if (tax == 1) {
                CHECK(unrest_state(rules().unrest, settled) == UnrestState::Calm);
            } else {
                CHECK(unrest_state(rules().unrest, settled) >= UnrestState::Agitation);
                CHECK(settled.raw() < 90 * kOne);
            }
        }
    }
}

TEST_CASE("unrest: a riot costs control and money, the province recovers, and the books balance") {
    World world = make_world(8);
    for (std::uint32_t p = 0; p < 8; ++p) {
        world.economy_columns_mut().tax_policy[p] = 3;
    }
    Simulation sim(std::move(world));
    install_economy_systems(sim, rules());
    run_months(sim, 240);
    const World& w = sim.world();
    // 20 years of excessive taxes in 8 provinces: riots happened, and each one is in the log
    const std::size_t riots = incidents_of(w, IncidentKind::Riot);
    CHECK(riots > 0);
    CHECK(w.economy().accounts.open_line(1, LedgerCode::RaidLoss) <= 0);
    Kopecks looted = 0;
    for (std::size_t m = 0; m < kLedgerRingMonths; ++m) {
        looted += w.economy().accounts.ring_lines(1, m)[*ledger_index(LedgerCode::RaidLoss)];
    }
    CHECK(looted < 0);  // the last twelve months were not riot-free
    for (std::uint32_t p = 0; p < 8; ++p) {
        CHECK(w.economy().provinces.control[p].raw() > 40 * kOne);  // control recovers between riots
    }
    require_valid(w);
}

TEST_CASE("unrest: the economy that rioted is the same economy on a second run and after a copy") {
    const auto build = [] {
        World world = make_world(12);
        for (std::uint32_t p = 0; p < 12; ++p) {
            world.economy_columns_mut().tax_policy[p] = static_cast<std::uint8_t>(2 + p % 2);
        }
        return world;
    };
    Simulation a(build());
    install_economy_systems(a, rules());
    Simulation b(build());
    install_economy_systems(b, rules());
    run_months(a, 60);
    run_months(b, 60);
    CHECK(a.world().compute_hash() == b.world().compute_hash());
    CHECK(a.world().economy() == b.world().economy());
    REQUIRE(incidents_of(a.world(), IncidentKind::Riot) > 0);

    // a copy of the world in the middle carries the stream of the riots with it
    Simulation c(build());
    install_economy_systems(c, rules());
    c.restore_state(World(a.world()), {});
    run_months(a, 36);
    run_months(c, 36);
    CHECK(a.world().compute_hash() == c.world().compute_hash());
    CHECK(a.world().economy() == c.world().economy());
}

TEST_CASE("riot: month lengths of 28, 29, 30 and 31 days, Julian leap years included: one draw a month") {
    // 56 provinces, two per slot day, all in riot state with no chance of a riot: every month each one
    // draws exactly once, so the stream stands where 56 x months calls of bounded(1000) put it
    EconomyRules r = rules();
    r.unrest.riot_chance_min_permille = 0;
    r.unrest.riot_chance_max_permille = 0;
    r.unrest.inertia_permille = 0;
    r.mood.inertia_permille = 0;
    r.control.control_inertia_permille = 0;
    for (const int start_year : {1703, 1707, 1799}) {  // 1704, 1708 and 1800 are Julian leap years
        constexpr std::uint32_t kProvinces = 56;
        World world = make_world(kProvinces, Date::from_ymd(start_year, 12, 31));
        for (std::uint32_t p = 0; p < kProvinces; ++p) {
            // below the threshold for some, at or above for others: only the latter draw
            world.economy_columns_mut().unrest[p] = points(p % 4 == 0 ? 40 : 60);
        }
        constexpr int kDrawing = kProvinces - kProvinces / 4;  // 42 of 56
        World reference(Date::from_ymd(1702, 12, 31), 5);
        Pcg32& expected = reference.rng().stream("economy.unrest");
        Simulation sim(std::move(world));
        install_economy_systems(sim, r);
        bool saw_february_of_29 = false;
        for (int month = 1; month <= 12; ++month) {
            const Date first = sim.world().today() + 1;
            saw_february_of_29 =
                saw_february_of_29 || (first.month() == 2 && Date::days_in_month(first.year(), 2) == 29);
            run_months(sim, 1);
            for (int i = 0; i < kDrawing; ++i) {
                (void)expected.bounded(1000);
            }
            REQUIRE(sim.world().rng().save() == reference.rng().save());
        }
        CHECK(saw_february_of_29);
        require_valid(sim.world());
    }
}

TEST_CASE("riot: the number of draws does not depend on who rioted") {
    // the same world twice with different chances (and so different riots): the draws are the same count
    const auto draws = [](std::int32_t chance) {
        EconomyRules r = rules();
        r.unrest.riot_chance_min_permille = chance;
        r.unrest.riot_chance_max_permille = chance;
        r.unrest.inertia_permille = 0;
        r.mood.inertia_permille = 0;
        r.control.control_inertia_permille = 0;
        World world = make_world(28);
        for (std::uint32_t p = 0; p < 28; ++p) {
            world.economy_columns_mut().unrest[p] = points(70);
        }
        Simulation sim(std::move(world));
        install_economy_systems(sim, r);
        run_months(sim, 6);
        World reference(Date::from_ymd(1702, 12, 31), 5);
        Pcg32& expected = reference.rng().stream("economy.unrest");
        for (int i = 0; i < 28 * 6; ++i) {
            (void)expected.bounded(1000);
        }
        return std::pair<bool, std::size_t>{sim.world().rng().save() == reference.rng().save(),
                                            incidents_of(sim.world(), IncidentKind::Riot)};
    };
    const auto few = draws(0);
    const auto some = draws(300);
    const auto all = draws(1000);
    CHECK(few.first);
    CHECK(some.first);
    CHECK(all.first);
    CHECK(few.second == 0);
    CHECK(some.second > 0);
    CHECK(some.second < all.second);
    CHECK(all.second == 28 * 6);
}

TEST_CASE("unrest: explain_unrest is the target the step used") {
    World world = make_world(1);
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        world.economy_columns_mut().mood[group_row(0, static_cast<Group>(g))] = points(-7);
    }
    world.economy_columns_mut().control[0] = points(25);
    const UnrestInput in = unrest_input(world, ProvinceId(0));
    const Breakdown b = explain_unrest(rules(), in);
    REQUIRE(b.is_consistent());
    CHECK(b.total() == compute_unrest(rules(), in).target.raw());
    CHECK(b.find(term::kUnrestControl) != kNoNode);
}

TEST_CASE("unrest: the mood the unrest reads is the mood the same slot has just written") {
    // one slot of one province: the unrest after the slot is computed from the NEW moods
    EconomyRules r = rules();
    r.control.control_inertia_permille = 0;
    r.control.corruption_inertia_permille = 0;
    r.unrest.riot_chance_min_permille = 0;
    r.unrest.riot_chance_max_permille = 0;
    r.consumption.wealth_inertia_permille = 0;
    World world = make_world(1);
    world.economy_columns_mut().tax_policy[0] = 3;
    world.economy_columns_mut().control[0] = points(70);
    const MoodInput mood_in = mood_input(world, ProvinceId(0));
    const ProvinceMood new_mood = compute_province_mood(r, mood_in);
    UnrestInput unrest_in = unrest_input(world, ProvinceId(0));
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        unrest_in.mood[g] = new_mood.group[g].mood;
    }
    const Fixed32 expected = compute_unrest(r, unrest_in).unrest;
    Simulation sim(std::move(world));
    install_economy_systems(sim, r);
    sim.run_days(1);  // province 0 is processed on the 1st
    CHECK(sim.world().economy().provinces.unrest[0] == expected);
    CHECK(sim.world().economy().groups.mood[group_row(0, Group::Peasants)] == new_mood.group[3].mood);
    CHECK(expected.raw() > 0);
}

TEST_CASE("riot: a save in the middle of a riotous run resumes identically, the stream of riots included") {
    const auto build = [] {
        World world = make_world(20);
        for (std::uint32_t p = 0; p < 20; ++p) {
            world.economy_columns_mut().tax_policy[p] = 3;
            world.economy_columns_mut().unrest[p] = points(55 + p % 20);
        }
        return world;
    };
    for (const int days_before_save : {17, 45, 400}) {
        CAPTURE(days_before_save);
        Simulation original(build());
        install_economy_systems(original, rules());
        original.run_days(days_before_save);
        REQUIRE(incidents_of(original.world(), IncidentKind::Riot) > 0);

        const auto file = serialize_save(original.world(), original.pending_internal());
        Simulation resumed(build());
        install_economy_systems(resumed, rules());
        auto loaded = deserialize_save(file);
        REQUIRE(loaded.has_value());
        resumed.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
        CHECK(resumed.world().compute_hash() == original.world().compute_hash());
        CHECK(resumed.world().rng().save() == original.world().rng().save());
        for (int step = 0; step < 4; ++step) {
            original.run_days(120);
            resumed.run_days(120);
            CHECK(resumed.world().compute_hash() == original.world().compute_hash());
        }
        CHECK(resumed.world().economy() == original.world().economy());
        CHECK(resumed.world().rng().save() == original.world().rng().save());
        require_valid(resumed.world());
    }
}
