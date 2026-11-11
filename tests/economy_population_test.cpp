#include "pat/core/rng.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/population.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// E9-T06: monthly growth of the population and the system `economy.provinces`
// (docs/dev/economy-model.md sections 7 and 9, formula F1; GDD 03 sections 4.2 and 14.1).

using namespace pat::sim;
using namespace pat::sim::economy;
using pat::core::Fixed32;
using pat::core::Pcg32;
using pat::core::StringId;

namespace {

const EconomyRules& rules() {
    static const EconomyRules value = [] {
        EconomyRules r = EconomyRules::defaults();
        // E9-T10: the administration is frozen here, control and corruption stay as the test sets them
        // (the dynamics of F7 have their own tests in economy_administrator_test.cpp)
        r.control.control_inertia_permille = 0;
        r.control.corruption_inertia_permille = 0;
        // E9-T11: mood, unrest and riots are frozen too (their dynamics have their own tests)
        r.mood.inertia_permille = 0;
        r.unrest.inertia_permille = 0;
        r.unrest.riot_chance_min_permille = 0;
        r.unrest.riot_chance_max_permille = 0;
        return r;
    }();
    return value;
}

GrowthInput input_of(std::uint32_t size, Group group = Group::Peasants) {
    GrowthInput in;
    in.group = group;
    in.size = size;
    return in;
}

void require_valid(const World& world) {
    const auto problems = world.check_invariants();
    for (const auto& problem : problems) {
        MESSAGE(problem);
    }
    REQUIRE(problems.empty());
}

/// The start of a world whose first tick is 1 January `year`.
World empty_world(int year) {
    World world(Date::from_ymd(year - 1, 12, 31), 5);
    (void)world.create_dynasty(StringId(1));
    return world;
}

/// `provinces` provinces of development `dev(p)`; every group gets `share_permille` of the capacity
/// of its province (peasants 66, townsfolk 17, cossacks 10, nobility 4, clergy 3: 10% in all with a
/// share of 1000), wealth and growth modifiers vary with the province.
World peaceful_world(std::uint32_t provinces, int year = 1703, std::int32_t fill_permille = 1000) {
    World world = empty_world(year);
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.create_province(StringId(100 + p), DynastyId(0), Fixed32::from_int(1));
    }
    REQUIRE(world.enable_economy());
    const EconomyColumnsMut c = world.economy_columns_mut();
    for (std::uint32_t p = 0; p < provinces; ++p) {
        const ProvinceId id(p);
        const auto dev = static_cast<std::uint8_t>(2 + p % 7);
        world.set_province_development(id, dev);
        const std::int64_t cap = static_cast<std::int64_t>(rules().population.capacity_per_development) * dev;
        const auto part = [&](std::int64_t permille_of_cap) {
            return static_cast<std::uint32_t>(cap * permille_of_cap / 1000 * fill_permille / 1000);
        };
        (void)world.set_group_size(id, Group::Peasants, part(66));
        (void)world.set_group_size(id, Group::Townsfolk, part(17));
        (void)world.set_group_size(id, Group::Cossacks, part(10));
        (void)world.set_group_size(id, Group::Nobility, part(4));
        (void)world.set_group_size(id, Group::Clergy, part(3));
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            const std::size_t row = group_row(p, static_cast<Group>(g));
            const std::int32_t wealth_steps[] = {80, 100, 140};
            c.wealth[row] = Fixed32::from_int(wealth_steps[p % 3]);
            c.growth_modifier[row] = p % 5 == 3 ? 2000 : (p % 5 == 4 ? -1000 : 0);
        }
        // E9-T07: the province feeds itself (a fifth to spare) and its workforce is the reference of
        // the labour factor, so that the production of the system keeps wealth, and with it growth,
        // where the tests of the population put them.
        std::array<std::uint32_t, kGroupCount> sizes{};
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            sizes[g] = world.economy().groups.size[group_row(p, static_cast<Group>(g))];
        }
        const Quantity need = compute_need(rules().consumption, sizes, kGrainGood);
        c.capacity[p * kMaxGoods + kGrainGood] = Fixed32::from_raw(
            static_cast<std::int32_t>(std::min<Quantity>(need * 1200 / 950, kStockMax.raw())));
        c.labour_base[p] = sizes[index_of(Group::Peasants)] + sizes[index_of(Group::Townsfolk)];
    }
    require_valid(world);
    return world;
}

Simulation economy_sim(World world) {
    Simulation sim(std::move(world));
    install_economy_systems(sim, rules());
    return sim;
}

std::uint64_t economy_part(const World& world) {
    return world.compute_hash().ext[static_cast<std::size_t>(ExtPart::Economy)];
}

std::array<std::uint64_t, kGroupCount> people_by_group(const World& world) {
    std::array<std::uint64_t, kGroupCount> totals{};
    const auto& sizes = world.economy().groups.size;
    for (std::size_t row = 0; row < sizes.size(); ++row) {
        totals[row % kGroupCount] += sizes[row];
    }
    return totals;
}

std::uint64_t people(const World& world) {
    std::uint64_t total = 0;
    for (const std::uint64_t part : people_by_group(world)) {
        total += part;
    }
    return total;
}

int days_in_year(int year) {
    return Date::is_leap_year(year) ? 366 : 365;
}

}  // namespace

// ---- the formula ---------------------------------------------------------------------------------

TEST_CASE("population growth: the default rules are the half a percent a year of the GDD") {
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        CHECK(rules().population.birth_ppm_year[g] - rules().population.death_ppm_year[g] == 5000);
    }
}

TEST_CASE("population growth: a month is the yearly rate over twelve, with the remainder kept") {
    // 1 000 000 people at +5000 ppm a year: 416 666 666 micro-people a month.
    const GrowthResult month = compute_group_growth(rules().population, input_of(1'000'000));
    CHECK(month.net_ppm_year == 5000);
    CHECK(month.crowding_permille == 1000);
    CHECK(month.delta_people == 416);
    CHECK(month.new_carry == 666'666);
    CHECK(month.new_size == 1'000'416);
    CHECK(month.lost_people == 0);
}

TEST_CASE("population growth: a rate too small for one person a month is not lost") {
    // 100 people: 41 666 micro-people a month, one whole person every 24 months and a bit.
    GrowthInput in = input_of(100);
    std::int64_t gained = 0;
    int first_person_month = 0;
    for (int month = 1; month <= 25; ++month) {
        const GrowthResult r = compute_group_growth(rules().population, in);
        gained += r.delta_people;
        if (gained == 1 && first_person_month == 0) {
            first_person_month = month;
        }
        in.carry = r.new_carry;  // the size is held at 100 on purpose: only the remainder moves
    }
    CHECK(first_person_month == 25);
    CHECK(gained == 1);
    CHECK(in.carry == 41'650);  // 25 x 41 666 - 10^6
}

TEST_CASE("population growth: shrinkage keeps its remainder with the sign of the debt") {
    GrowthInput in = input_of(100);
    in.growth_modifier_ppm = -10'000;  // net -5000 ppm
    std::int64_t lost = 0;
    for (int month = 1; month <= 25; ++month) {
        const GrowthResult r = compute_group_growth(rules().population, in);
        CHECK(r.net_ppm_year == -5000);
        CHECK(r.crowding_permille == 1000);  // crowding never slows shrinkage
        lost += r.delta_people;
        in.carry = r.new_carry;
        CHECK(in.carry <= 0);
        CHECK(in.carry > -static_cast<std::int32_t>(kMicroPerPerson));
    }
    CHECK(lost == -1);
    CHECK(in.carry == -41'650);
}

TEST_CASE("population growth: whole people and remainder together are exactly what the rate asked for") {
    // The only rounding is the truncation of size x net / 12 to a micro-person: nothing is lost or
    // created by the split into whole people and remainder.
    Pcg32 rng(2024, 1);
    for (int i = 0; i < 20'000; ++i) {
        GrowthInput in = input_of(rng.bounded(3'000'000));
        in.group = static_cast<Group>(rng.bounded(kGroupCount));
        in.carry = rng.range(-999'999, 999'999);
        in.wealth = Fixed32::from_raw(rng.range(0, 300 * Fixed32::one_raw));
        in.growth_modifier_ppm = rng.range(-20'000, 20'000);
        in.development = rng.range(1, kMaxDevelopment);
        in.province_persons = rng.bounded(400'000);
        const GrowthResult r = compute_group_growth(rules().population, in);
        const std::int64_t asked = static_cast<std::int64_t>(in.carry) +
                                   static_cast<std::int64_t>(in.size) * r.net_ppm_year / kMonthsPerYear;
        REQUIRE(r.delta_people * kMicroPerPerson + r.new_carry == asked);
        REQUIRE(r.new_carry > -static_cast<std::int32_t>(kMicroPerPerson));
        REQUIRE(r.new_carry < static_cast<std::int32_t>(kMicroPerPerson));
        REQUIRE((r.new_carry == 0 || (r.new_carry > 0) == (asked > 0)));
        REQUIRE(static_cast<std::int64_t>(r.new_size) == static_cast<std::int64_t>(in.size) + r.delta_people);
    }
}

TEST_CASE("population growth: wealth above normal helps, below normal hurts, by the rule") {
    const auto net = [](std::int32_t wealth_points) {
        GrowthInput in = input_of(1000);
        in.wealth = Fixed32::from_int(wealth_points);
        return compute_group_growth(rules().population, in).net_ppm_year;
    };
    const std::int32_t per_point = rules().population.wealth_growth_ppm_per_point;
    CHECK(net(100) == 5000);
    CHECK(net(150) == 5000 + 50 * per_point);
    CHECK(net(50) == 5000 - 50 * per_point);
    CHECK(net(0) == 5000 - 100 * per_point);
    CHECK(net(300) == 5000 + 200 * per_point);
    // half a point is half a rate step (truncated toward zero)
    GrowthInput in = input_of(1000);
    in.wealth = Fixed32::from_raw(Fixed32::one_raw * 100 + Fixed32::one_raw / 2);
    CHECK(compute_group_growth(rules().population, in).net_ppm_year == 5000 + per_point / 2);
}

TEST_CASE("population growth: crowding slows growth up to a stop at the capacity, never shrinkage") {
    const std::int64_t cap = static_cast<std::int64_t>(rules().population.capacity_per_development) * 4;
    GrowthInput in = input_of(10'000);
    in.development = 4;
    const auto at = [&](std::uint64_t persons, std::int32_t modifier = 0) {
        GrowthInput x = in;
        x.province_persons = persons;
        x.growth_modifier_ppm = modifier;
        return compute_group_growth(rules().population, x);
    };
    CHECK(at(0).net_ppm_year == 5000);
    CHECK(at(static_cast<std::uint64_t>(cap / 2)).net_ppm_year == 2500);
    CHECK(at(static_cast<std::uint64_t>(cap / 2)).crowding_permille == 500);
    CHECK(at(static_cast<std::uint64_t>(cap)).net_ppm_year == 0);
    CHECK(at(static_cast<std::uint64_t>(cap) * 3).net_ppm_year == 0);
    // a shrinking group is not slowed by crowding, a crowded province does not make it worse either
    CHECK(at(static_cast<std::uint64_t>(cap) * 3, -10'000).net_ppm_year == -5000);
    // the development moves the capacity
    GrowthInput big = in;
    big.development = 10;
    big.province_persons = static_cast<std::uint64_t>(cap);
    CHECK(compute_group_growth(rules().population, big).net_ppm_year == 5000 * 6 / 10);
}

TEST_CASE("population growth: losses are taken from the group and never below nobody") {
    GrowthInput in = input_of(1000);
    in.losses = 30;
    const GrowthResult r = compute_group_growth(rules().population, in);
    CHECK(r.lost_people == 30);
    CHECK(r.new_size == static_cast<std::uint32_t>(1000 + r.delta_people - 30));

    in = input_of(10);
    in.losses = 50;
    const GrowthResult all = compute_group_growth(rules().population, in);
    CHECK(all.new_size == 0);
    CHECK(all.lost_people == 10);

    // an empty group stays empty, whatever the rate
    in = input_of(0);
    in.growth_modifier_ppm = 500'000;
    const GrowthResult none = compute_group_growth(rules().population, in);
    CHECK(none.new_size == 0);
    CHECK(none.new_carry == 0);
}

TEST_CASE("population growth: the breakdown adds up, names its terms and changes nothing") {
    GrowthInput in = input_of(50'000, Group::Townsfolk);
    in.development = 4;
    in.province_persons = 40'000;  // 60% room
    in.wealth = Fixed32::from_int(125);
    in.growth_modifier_ppm = 700;
    Breakdown b;
    const GrowthResult with = compute_group_growth(rules().population, in, &b);
    const GrowthResult without = compute_group_growth(rules().population, in);
    CHECK(with == without);

    REQUIRE_FALSE(b.truncated());
    CHECK(b.is_consistent());
    CHECK(b.root().code == term::kGrowthNet);
    CHECK(b.root().unit == Unit::Ppm);
    CHECK(b.total() == with.net_ppm_year);
    const std::size_t g = index_of(Group::Townsfolk);
    CHECK(b.node(b.find(term::kGrowthBirth)).value == rules().population.birth_ppm_year[g]);
    CHECK(b.node(b.find(term::kGrowthDeath)).value == -rules().population.death_ppm_year[g]);
    CHECK(b.node(b.find(term::kGrowthWealth)).value == 25 * rules().population.wealth_growth_ppm_per_point);
    CHECK(b.node(b.find(term::kGrowthModifier)).value == 700);
    const NodeIndex crowding = b.find(term::kGrowthCrowding);
    REQUIRE(crowding != kNoNode);
    CHECK(b.node(crowding).value < 0);
    CHECK(b.node(crowding).factor_permille == with.crowding_permille);
    CHECK(b.find(term::kGrowthMigration) == kNoNode);  // M2 terms are empty and left out
    CHECK(b.find(term::kGrowthFamine) == kNoNode);
    CHECK_FALSE(term_key(term::kGrowthNet).empty());

    // without crowding and without wealth/modifier only birth and death remain
    Breakdown plain;
    (void)compute_group_growth(rules().population, input_of(10), &plain);
    CHECK(plain.size() == 3);
    CHECK(plain.is_consistent());
}

TEST_CASE(
    "population growth: a province is computed from the state of its start, group order is irrelevant") {
    World world = peaceful_world(1);
    const ProvinceId p(0);
    const std::uint64_t before = world.population_persons(p);
    const World copy = world;

    const ProvinceGrowth growth = compute_province_growth(world, rules().population, p);
    CHECK(world.economy() == copy.economy());  // computing writes nothing
    CHECK(world.compute_hash() == copy.compute_hash());
    CHECK(growth.persons_before == before);
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        CHECK(growth.groups[g] ==
              compute_group_growth(rules().population, growth_input(world, p, static_cast<Group>(g))));
    }
    CHECK(growth.groups[index_of(Group::Natives)].new_size == 0);

    const ProvinceGrowth applied = step_province_population(world, rules().population, p);
    CHECK(applied == growth);
    CHECK(world.population_persons(p) == growth.persons_after);
    CHECK(world.provinces().population[0] ==
          Fixed32::from_ratio(static_cast<std::int64_t>(growth.persons_after), 1000));
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        CHECK(world.economy().groups.size[g] == growth.groups[g].new_size);
        CHECK(world.economy().groups.growth_carry[g] == growth.groups[g].new_carry);
    }
    require_valid(world);
}

TEST_CASE("population growth: a world without economy, an unknown province and the explanation") {
    World plain(Date::from_ymd(1703, 1, 1), 1);
    (void)plain.create_province(StringId(1), DynastyId(), Fixed32::from_int(5));
    CHECK(step_province_population(plain, rules().population, ProvinceId(0)) == ProvinceGrowth{});
    CHECK(plain.provinces().population[0] == Fixed32::from_int(5));

    World world = peaceful_world(2);
    CHECK(compute_province_growth(world, rules().population, ProvinceId(9)) == ProvinceGrowth{});
    const Breakdown tooltip = explain_group_growth(world, rules().population, ProvinceId(1), Group::Peasants);
    CHECK(tooltip.is_consistent());
    CHECK(tooltip.total() == compute_province_growth(world, rules().population, ProvinceId(1))
                                 .groups[index_of(Group::Peasants)]
                                 .net_ppm_year);
}

// ---- the system ----------------------------------------------------------------------------------

TEST_CASE("economy.provinces: every province is processed exactly once a month, in months of any length") {
    constexpr std::uint32_t kProvinces = 61;  // not a multiple of 28: the slots are not even
    // The Julian leap years 1704, 1708, 1712 and the century year 1800, which is leap in Julian only.
    for (const int first_year : {1703, 1799}) {
        const int last_year = first_year == 1703 ? 1712 : 1800;
        Simulation sim = economy_sim(peaceful_world(kProvinces, first_year));
        const auto months = static_cast<std::size_t>((last_year - first_year + 1) * 12);
        std::vector<int> processed(months * kProvinces, 0);
        std::array<int, 32> months_of_length{};
        std::array<int, 32> idle_days{};  // days 29, 30, 31 on which nobody was processed
        std::vector<Date> last(kProvinces, kNoDate);

        const int days = [&] {
            int total = 0;
            for (int y = first_year; y <= last_year; ++y) {
                total += days_in_year(y);
            }
            return total;
        }();
        for (int d = 0; d < days; ++d) {
            (void)sim.tick();
            const Date today = sim.world().today();
            const auto month_index =
                static_cast<std::size_t>((today.year() - first_year) * 12 + today.month() - 1);
            int today_count = 0;
            for (std::uint32_t p = 0; p < kProvinces; ++p) {
                const Date seen = sim.world().economy().provinces.last_slot_date[p];
                if (seen != last[p]) {
                    REQUIRE(seen == today);  // set to the day of the slot, never to another date
                    REQUIRE(today.day() == static_cast<int>(p % 28) + 1);
                    REQUIRE(is_monthly_slot(p, today));
                    ++processed[month_index * kProvinces + p];
                    ++today_count;
                    last[p] = seen;
                }
            }
            if (today.day() == Date::days_in_month(today.year(), today.month())) {
                ++months_of_length[static_cast<std::size_t>(today.day())];
            }
            if (today.day() > 28) {
                CHECK(today_count == 0);
                ++idle_days[static_cast<std::size_t>(today.day())];
            }
        }
        for (std::size_t m = 0; m < months; ++m) {
            for (std::uint32_t p = 0; p < kProvinces; ++p) {
                INFO("month index " << m << " province " << p);
                REQUIRE(processed[m * kProvinces + p] == 1);
            }
        }
        // every kind of month was covered: 28, 29, 30 and 31 days
        CHECK(months_of_length[28] > 0);
        CHECK(months_of_length[29] > 0);
        CHECK(months_of_length[30] > 0);
        CHECK(months_of_length[31] > 0);
        CHECK(idle_days[31] > 0);
        require_valid(sim.world());
    }
}

TEST_CASE("economy.provinces: the stagger visits a province on day index % 28 + 1 and changes only it") {
    Simulation sim = economy_sim(peaceful_world(60));
    const World start = sim.world();
    (void)sim.tick();  // 1 January: provinces 0, 28, 56
    for (std::uint32_t p = 0; p < 60; ++p) {
        const bool slot = p % 28 == 0;
        INFO("province " << p);
        CHECK((sim.world().economy().provinces.last_slot_date[p] == sim.world().today()) == slot);
        bool changed = false;
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            const std::size_t row = group_row(p, static_cast<Group>(g));
            changed =
                changed ||
                sim.world().economy().groups.growth_carry[row] != start.economy().groups.growth_carry[row] ||
                sim.world().economy().groups.size[row] != start.economy().groups.size[row];
        }
        CHECK(changed == slot);
    }
}

TEST_CASE("economy.provinces: the system registers as the third economy system and needs an economy") {
    Simulation sim = economy_sim(peaceful_world(3));
    REQUIRE(sim.system_count() == 2);  // settlement (E9-T09) in front of the provinces
    (void)sim.tick();
    CHECK(sim.last_report().systems[0].name == kSettlementSystemName);
    CHECK(sim.last_report().systems[0].cadence == Cadence::Monthly);
    CHECK(sim.last_report().systems[1].name == kProvincesSystemName);
    CHECK(sim.last_report().systems[1].cadence == Cadence::Daily);
    CHECK(sim.last_report().systems[1].ran);

    // with the economy off the system does nothing, however long it runs
    World plain = empty_world(1703);
    (void)plain.create_province(StringId(1), DynastyId(0), Fixed32::from_int(7));
    const World before = plain;
    Simulation off = economy_sim(std::move(plain));
    off.run_days(120);
    CHECK(off.world().provinces().population == before.provinces().population);
    CHECK_FALSE(off.world().economy_present());
}

TEST_CASE("economy.provinces: fifty years of peace stay in the corridor of the GDD") {
    constexpr int kYears = 50;
    Simulation sim = economy_sim(peaceful_world(20));
    const std::array<std::uint64_t, kGroupCount> first = people_by_group(sim.world());
    std::array<std::uint64_t, kGroupCount> previous = first;
    std::uint64_t previous_total = people(sim.world());
    const std::uint64_t first_total = previous_total;
    for (int year = 1703; year < 1703 + kYears; ++year) {
        sim.run_days(days_in_year(year));
        REQUIRE(sim.world().today() == Date::from_ymd(year, 12, 31));
        const auto now = people_by_group(sim.world());
        const std::uint64_t total = people(sim.world());
        INFO("year " << year);
        // GDD 03 section 14.1: total growth 0.3 ... 1.0 % a year, no group extinct, none exploding
        const double total_rate = static_cast<double>(total) / static_cast<double>(previous_total) - 1.0;
        CHECK(total_rate >= 0.003);
        CHECK(total_rate <= 0.010);
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            if (previous[g] == 0) {
                CHECK(now[g] == 0);  // the reserve slot stays empty
                continue;
            }
            const double rate = static_cast<double>(now[g]) / static_cast<double>(previous[g]) - 1.0;
            CHECK(rate >= 0.003);
            CHECK(rate <= 0.010);
        }
        previous = now;
        previous_total = total;
    }
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        if (first[g] == 0) {
            continue;
        }
        const double factor = static_cast<double>(previous[g]) / static_cast<double>(first[g]);
        CHECK(factor > 1.0);
        CHECK(factor < 1.7);  // 1.01 ^ 50 = 1.64: not exponential
    }
    const double all = static_cast<double>(previous_total) / static_cast<double>(first_total);
    CHECK(all > 1.16);  // 1.003 ^ 50
    CHECK(all < 1.65);  // 1.010 ^ 50
    // every province stays below its capacity and the books are right
    for (std::uint32_t p = 0; p < 20; ++p) {
        const std::int64_t cap = static_cast<std::int64_t>(rules().population.capacity_per_development) *
                                 sim.world().provinces().development[p];
        CHECK(static_cast<std::int64_t>(sim.world().population_persons(ProvinceId(p))) < cap);
    }
    require_valid(sim.world());
}

TEST_CASE("economy.provinces: a province at its capacity stops growing and one in decline keeps shrinking") {
    World world = peaceful_world(2, 1703, 10'000);  // 10x the 10% fill: right at the capacity
    const std::uint64_t crowded = world.population_persons(ProvinceId(0));
    const EconomyColumnsMut c = world.economy_columns_mut();
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        c.growth_modifier[group_row(1, static_cast<Group>(g))] = -9000;  // net -4000 ppm: a plague year
        c.wealth[group_row(1, static_cast<Group>(g))] = Fixed32::from_int(100);
    }
    const std::uint64_t declining = world.population_persons(ProvinceId(1));
    Simulation sim = economy_sim(std::move(world));
    sim.run_days(5 * 366);
    CHECK(sim.world().population_persons(ProvinceId(0)) == crowded);  // zero growth, no shrinkage
    CHECK(sim.world().population_persons(ProvinceId(1)) < declining);
    // -0.4% a year for five years, but never extinct and never below zero
    CHECK(sim.world().population_persons(ProvinceId(1)) > declining * 97 / 100);
    require_valid(sim.world());
}

TEST_CASE("economy.provinces: the same world runs to the same hash, state and explanation") {
    Simulation a = economy_sim(peaceful_world(40));
    Simulation b = economy_sim(peaceful_world(40));
    CHECK(economy_part(a.world()) == economy_part(b.world()));
    for (int step = 0; step < 7; ++step) {
        a.run_days(61);
        b.run_days(61);
        REQUIRE(a.world().compute_hash() == b.world().compute_hash());
        REQUIRE(a.world().economy_hash_parts() == b.world().economy_hash_parts());
    }
    CHECK(a.world().economy() == b.world().economy());
    CHECK(economy_part(a.world()) != economy_part(peaceful_world(40)));  // it moves
    for (std::uint32_t p = 0; p < 40; p += 7) {
        const Breakdown x =
            explain_group_growth(a.world(), rules().population, ProvinceId(p), Group::Peasants);
        const Breakdown y =
            explain_group_growth(b.world(), rules().population, ProvinceId(p), Group::Peasants);
        CHECK(x == y);
        CHECK(x.is_consistent());
    }
    // another seed of nothing: the system uses no random numbers, so even the RNG state is untouched
    CHECK(a.world().compute_hash().rng == peaceful_world(40).compute_hash().rng);
}

TEST_CASE("economy.provinces: a save in the middle of a month resumes identically") {
    for (const int days_before_save : {0, 1, 14, 45, 100, 200}) {
        CAPTURE(days_before_save);
        Simulation original = economy_sim(peaceful_world(70));
        original.run_days(days_before_save);
        const auto file = serialize_save(original.world(), original.pending_internal());

        Simulation resumed = economy_sim(peaceful_world(70));
        auto loaded = deserialize_save(file);
        REQUIRE(loaded.has_value());
        resumed.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
        REQUIRE(resumed.world().compute_hash() == original.world().compute_hash());

        for (int step = 0; step < 5; ++step) {
            original.run_days(100);
            resumed.run_days(100);
            REQUIRE(resumed.world().compute_hash() == original.world().compute_hash());
        }
        CHECK(resumed.world().economy() == original.world().economy());
        require_valid(resumed.world());
    }
}

TEST_CASE("economy.provinces: it is not part of the demo scenario, whose run does not change") {
    demo::Config config;
    config.seed = 42;
    Simulation plain = demo::make_simulation(config);
    (void)plain.tick();
    for (const SystemRun& run : plain.last_report().systems) {
        CHECK(std::string(run.name).rfind("economy.", 0) != 0);
    }
    // Installing it into a world without economy changes nothing (it returns at once).
    Simulation with_system = demo::make_simulation(config);
    install_economy_systems(with_system, rules());
    Simulation reference = demo::make_simulation(config);
    with_system.run_days(800);
    reference.run_days(800);
    CHECK(with_system.world().compute_hash() == reference.world().compute_hash());
    CHECK_FALSE(with_system.world().economy_present());
}
