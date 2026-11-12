#include "pat/core/bytes.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/command_log.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/invariants.hpp"
#include "pat/sim/economy/policy.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/recruits.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

// E9-T13: the recruit potential of a province (F11), the monthly regeneration of the pool (step 6 of the
// slot), the command ConscriptRecruits (tag 34) and its effects: the pool, the groups, the manpower
// reserve of the owner, the output (ECO-A2), the log, replays, saves and the invariants.

using namespace pat::sim;
using namespace pat::sim::economy;
using pat::core::Fixed32;
using pat::core::StringId;

namespace {

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
    const auto broken = check_rules_invariants(world, rules());
    for (const auto& problem : broken) {
        MESSAGE(problem);
    }
    REQUIRE(broken.empty());
}

/// Three dynasties; `provinces` provinces held by dynasty 0, except the last one when `crown_last`
/// (the crown's); peasants 8000, townsfolk 1500, cossacks 500; the levels of the rules are known.
World make_world(std::uint32_t provinces,
                 Date start = Date::from_ymd(1702, 12, 31),
                 bool crown_last = false) {
    World world(start, 5);
    for (std::uint32_t d = 0; d < 3; ++d) {
        (void)world.create_dynasty(StringId(1 + d));
    }
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.create_province(StringId(100 + p), DynastyId(0), Fixed32::from_int(1));
    }
    REQUIRE(world.enable_economy());
    bind_policy_levels(world, rules());
    if (crown_last && provinces > 0) {
        REQUIRE(world.set_province_owner_kind(ProvinceId(provinces - 1), OwnerKind::Crown));
    }
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.set_group_size(ProvinceId(p), Group::Peasants, 8000);
        (void)world.set_group_size(ProvinceId(p), Group::Townsfolk, 1500);
        (void)world.set_group_size(ProvinceId(p), Group::Cossacks, 500);
    }
    require_valid(world);
    return world;
}

Simulation economy_sim(World world) {
    Simulation sim(std::move(world));
    install_economy_systems(sim, rules());
    return sim;
}

Command conscript(DynastyId actor, std::uint32_t province, std::uint32_t count) {
    return Command{actor, cmd::ConscriptRecruits{ProvinceId(province), count}};
}

Rejection verdict(const World& world, const Command& command) {
    const auto result = validate(world, command);
    return result.has_value() ? Rejection::None : result.error().reason;
}

std::uint32_t pool(const World& world, std::uint32_t province) {
    return world.economy().provinces.recruit_pool[province];
}
std::uint32_t reserve(const World& world, std::uint32_t dynasty) {
    return world.economy().dynasties.manpower_reserve[dynasty];
}
std::uint32_t size_of(const World& world, std::uint32_t province, Group group) {
    return world.economy().groups.size[province * kGroupCount + index_of(group)];
}
std::int32_t mood_raw(const World& world, std::uint32_t province, Group group) {
    return world.economy().groups.mood[province * kGroupCount + index_of(group)].raw();
}
std::uint32_t potential(const World& world, std::uint32_t province) {
    return compute_recruit_cap(rules(), recruit_input(world, rules(), ProvinceId(province)));
}

RecruitInput plain_input() {
    RecruitInput in;
    in.size[index_of(Group::Peasants)] = 8000;
    in.size[index_of(Group::Townsfolk)] = 1500;
    in.size[index_of(Group::Cossacks)] = 500;
    in.control = Fixed32::from_int(100);
    in.level = 1;
    return in;
}

}  // namespace

// ---- the formula ------------------------------------------------------------------------------------

TEST_CASE("F11: the potential is the share of the groups x the policy x the control, nothing else") {
    // defaults: townsfolk 1.5 %, peasants 3 %, cossacks 5 %; levels x0.5, x1.0, x1.6
    RecruitInput in = plain_input();
    // 1500 x 15 / 1000 = 22 (truncated), 8000 x 30 / 1000 = 240, 500 x 50 / 1000 = 25
    CHECK(compute_recruit_cap(rules(), in) == 287);
    in.level = 0;
    CHECK(compute_recruit_cap(rules(), in) == 143);
    in.level = 2;
    CHECK(compute_recruit_cap(rules(), in) == 459);
    in.level = 200;  // past the table: the last level
    CHECK(compute_recruit_cap(rules(), in) == 459);
    in.level = 1;
    in.control = Fixed32::from_int(50);  // 500 per-mille
    CHECK(compute_recruit_cap(rules(), in) == 143);
    in.control = Fixed32::from_int(0);
    CHECK(compute_recruit_cap(rules(), in) == 0);
    // nobility, clergy and natives give nobody
    RecruitInput others;
    others.size[index_of(Group::Nobility)] = 5000;
    others.size[index_of(Group::Clergy)] = 5000;
    others.size[index_of(Group::Natives)] = 5000;
    CHECK(compute_recruit_cap(rules(), others) == 0);
    // a province in turmoil (control below the anarchy threshold) gives none; at the threshold it does
    in = plain_input();
    in.control = Fixed32::from_int(rules().control.anarchy_threshold);
    CHECK(compute_recruit_cap(rules(), in) > 0);
    in.control = Fixed32::from_raw(Fixed32::from_int(rules().control.anarchy_threshold).raw() - 1);
    CHECK(compute_recruit_cap(rules(), in) == 0);
    // the hook of T14: the unpaid quota is taken from the potential, never below zero
    in = plain_input();
    in.quota_pending = 100;
    CHECK(compute_recruit_cap(rules(), in) == 187);
    in.quota_pending = 100'000;
    CHECK(compute_recruit_cap(rules(), in) == 0);
}

TEST_CASE("F11: the Breakdown explains the potential and adds up") {
    RecruitInput in = plain_input();
    in.level = 2;
    in.control = Fixed32::from_int(80);
    in.quota_pending = 10;
    Breakdown b;
    const std::uint32_t cap = compute_recruit_cap(rules(), in, &b);
    CHECK(b.root().code == term::kRecruitCap);
    CHECK(b.total() == cap);
    CHECK(b.is_consistent());
    CHECK(b.node(b.find(term::kRecruitBase)).value == 287);
    CHECK(b.node(b.find(term::kRecruitPolicy)).factor_permille == 1600);
    CHECK(b.node(b.find(term::kRecruitControl)).factor_permille == 800);
    CHECK(b.node(b.find(term::kRecruitQuota)).value == -10);
    CHECK(b.find(term::kRecruitAnarchy) == kNoNode);
    // a multiplier of exactly 1000 is left out
    Breakdown usual;
    (void)compute_recruit_cap(rules(), plain_input(), &usual);
    CHECK(usual.find(term::kRecruitPolicy) == kNoNode);
    CHECK(usual.find(term::kRecruitControl) == kNoNode);
    CHECK(usual.is_consistent());
    // the answer does not depend on whether an explanation was asked for
    CHECK(compute_recruit_cap(rules(), in) == cap);
    // turmoil is a step to zero
    in.control = Fixed32::from_int(5);
    Breakdown turmoil;
    CHECK(compute_recruit_cap(rules(), in, &turmoil) == 0);
    CHECK(turmoil.find(term::kRecruitAnarchy) != kNoNode);
    CHECK(turmoil.is_consistent());
}

TEST_CASE("the pool moves toward the potential, arrives exactly and never exceeds it") {
    RecruitInput in = plain_input();  // cap 287, regen 100 per-mille
    std::uint32_t last = 0;
    int months = 0;
    while (in.pool != 287 && months < 400) {
        const RecruitResult r = compute_recruit_pool(rules(), in);
        CHECK(r.cap == 287);
        CHECK(r.new_pool <= r.cap);
        CHECK(r.new_pool > in.pool);  // at least one person a month until it arrives
        in.pool = r.new_pool;
        CHECK(in.pool >= last);
        last = in.pool;
        ++months;
    }
    CHECK(in.pool == 287);
    CHECK(months < 400);
    CHECK(compute_recruit_pool(rules(), in).new_pool == 287);  // stays

    // the first month: a tenth of the gap
    RecruitInput fresh = plain_input();
    CHECK(compute_recruit_pool(rules(), fresh).new_pool == 28);

    // a cap that fell cuts the pool at once
    in.level = 0;
    CHECK(compute_recruit_pool(rules(), in).new_pool == 143);
    in.level = 1;
    in.pool = 287;
    in.control = Fixed32::from_int(5);
    CHECK(compute_recruit_pool(rules(), in).new_pool == 0);

    // no regeneration in the rules: the pool does not grow (and still never exceeds the cap)
    EconomyRules frozen = rules();
    frozen.recruit.regen_permille = 0;
    RecruitInput none = plain_input();
    none.pool = 10;
    CHECK(compute_recruit_pool(frozen, none).new_pool == 10);
    none.pool = 9999;
    CHECK(compute_recruit_pool(frozen, none).new_pool == 287);
    // regeneration 1000: the whole gap in one month
    frozen.recruit.regen_permille = 1000;
    none.pool = 0;
    CHECK(compute_recruit_pool(frozen, none).new_pool == 287);
}

TEST_CASE("split_conscription: proportional, the remainder in order, never more than a group has") {
    std::array<std::uint32_t, kGroupCount> size{};
    size[index_of(Group::Townsfolk)] = 1500;
    size[index_of(Group::Peasants)] = 8000;
    size[index_of(Group::Cossacks)] = 500;
    size[index_of(Group::Nobility)] = 77;  // never touched
    const auto take = split_conscription(size, 1000);
    CHECK(take[index_of(Group::Townsfolk)] == 150);
    CHECK(take[index_of(Group::Peasants)] == 800);
    CHECK(take[index_of(Group::Cossacks)] == 50);
    CHECK(take[index_of(Group::Nobility)] == 0);
    CHECK(take[index_of(Group::Clergy)] == 0);
    CHECK(take[index_of(Group::Natives)] == 0);

    // a remainder: 7 of 3 equal groups of 10 -> 2, 2, 2 and one more, to townsfolk first
    std::array<std::uint32_t, kGroupCount> equal{};
    equal[index_of(Group::Townsfolk)] = equal[index_of(Group::Peasants)] = equal[index_of(Group::Cossacks)] =
        10;
    const auto seven = split_conscription(equal, 7);
    CHECK(seven[index_of(Group::Townsfolk)] == 3);
    CHECK(seven[index_of(Group::Peasants)] == 2);
    CHECK(seven[index_of(Group::Cossacks)] == 2);

    // more than there are: cut to everyone; zero: nothing; nobody: nothing
    const auto all = split_conscription(equal, 1000);
    CHECK(all[index_of(Group::Townsfolk)] == 10);
    CHECK(all[index_of(Group::Peasants)] == 10);
    CHECK(all[index_of(Group::Cossacks)] == 10);
    const auto zero = split_conscription(equal, 0);
    CHECK(std::accumulate(zero.begin(), zero.end(), 0U) == 0);
    const auto nobody = split_conscription(std::array<std::uint32_t, kGroupCount>{}, 5);
    CHECK(std::accumulate(nobody.begin(), nobody.end(), 0U) == 0);

    // every count from 0 to the population: the sum is exact and no group goes below nothing
    std::array<std::uint32_t, kGroupCount> odd{};
    odd[index_of(Group::Townsfolk)] = 3;
    odd[index_of(Group::Peasants)] = 7;
    odd[index_of(Group::Cossacks)] = 1;
    for (std::uint32_t count = 0; count <= 11; ++count) {
        const auto t = split_conscription(odd, count);
        std::uint32_t sum = 0;
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            CHECK(t[g] <= odd[g]);
            sum += t[g];
        }
        CHECK(sum == count);
    }
}

// ---- the tag and the log ----------------------------------------------------------------------------

TEST_CASE("ConscriptRecruits: tag 34, the name, the log round-trips, the other tags stay") {
    const Command command = conscript(DynastyId(1), 7, 250);
    CHECK(static_cast<std::uint8_t>(type_of(command)) == 34);
    CHECK(to_string(CommandType::ConscriptRecruits) == "ConscriptRecruits");
    CHECK(static_cast<std::uint8_t>(CommandType::SetTaxPolicy) == 32);
    CHECK(static_cast<std::uint8_t>(CommandType::SetRecruitPolicy) == 33);
    CHECK(static_cast<std::uint8_t>(CommandType::AssignProvinceAdministrator) == 35);

    CommandLog log;
    LoggedCommand entry;
    entry.date = Date::from_ymd(1703, 3, 4);
    entry.source = CommandSource::External;
    entry.command = command;
    log.append(entry);
    entry.command = conscript(DynastyId(), 0, 0xFFFFFFFFU);
    entry.outcome = Rejection::InvalidState;
    log.append(entry);
    const auto bytes = log.serialize();
    const auto back = CommandLog::deserialize(bytes);
    REQUIRE(back.has_value());
    REQUIRE(back.value().entries().size() == 2);
    CHECK(back.value().entries()[0].command == command);
    CHECK(back.value().entries()[1].command == conscript(DynastyId(), 0, 0xFFFFFFFFU));
    CHECK(back.value().entries()[1].outcome == Rejection::InvalidState);
    CHECK(back.value().serialize() == bytes);
}

// ---- validate -----------------------------------------------------------------------------------------

TEST_CASE("ConscriptRecruits: every rejection and its reason") {
    World world = make_world(3, Date::from_ymd(1702, 12, 31), true);  // province 2 is the crown's
    world.economy_columns_mut().recruit_pool[0] = 100;
    world.economy_columns_mut().recruit_pool[2] = 100;

    CHECK(verdict(world, conscript(DynastyId(0), 0, 100)) == Rejection::None);
    CHECK(verdict(world, conscript(DynastyId(), 0, 1)) == Rejection::None);  // the system
    CHECK(verdict(world, conscript(DynastyId(0), 0, 0)) == Rejection::InvalidArgument);
    CHECK(verdict(world, conscript(DynastyId(0), 0, 101)) == Rejection::InvalidState);  // above the pool
    CHECK(verdict(world, conscript(DynastyId(0), 1, 1)) == Rejection::InvalidState);    // an empty pool
    CHECK(verdict(world, conscript(DynastyId(1), 0, 10)) == Rejection::NotAuthorized);  // not the owner
    CHECK(verdict(world, conscript(DynastyId(0), 99, 10)) == Rejection::UnknownEntity);
    // a province of the crown has no dynasty to hold the reserve: the system is told so, a dynasty
    // that does not own it is not allowed
    CHECK(verdict(world, conscript(DynastyId(), 2, 10)) == Rejection::InvalidState);
    CHECK(verdict(world, conscript(DynastyId(0), 2, 10)) == Rejection::NotAuthorized);

    // a pool larger than the people of the recruit groups (written by hand): refused, never negative
    world.economy_columns_mut().recruit_pool[1] = 1'000'000;
    CHECK(verdict(world, conscript(DynastyId(0), 1, 10'001)) == Rejection::InvalidState);
    CHECK(verdict(world, conscript(DynastyId(0), 1, 10'000)) == Rejection::None);
    // the reserve cannot overflow
    world.economy_columns_mut().manpower_reserve[0] = 0xFFFFFFFFU - 5;
    CHECK(verdict(world, conscript(DynastyId(0), 0, 6)) == Rejection::InvalidState);
    CHECK(verdict(world, conscript(DynastyId(0), 0, 5)) == Rejection::None);

    const World none(Date::from_ymd(1703, 1, 1), 1);
    CHECK(verdict(none, conscript(DynastyId(), 0, 1)) == Rejection::InvalidState);
}

TEST_CASE("ConscriptRecruits: a refused command changes nothing, an accepted one exactly the documented") {
    World w = make_world(2);
    w.economy_columns_mut().recruit_pool[0] = 300;
    const EconomyState e0 = w.economy();
    const Command command = conscript(DynastyId(0), 0, 100);
    REQUIRE(validate(w, command).has_value());
    apply(w, command);
    const EconomyState& e1 = w.economy();
    CHECK(reserve(w, 0) == 100);
    CHECK(reserve(w, 1) == 0);
    CHECK(pool(w, 0) == 200);
    CHECK(pool(w, 1) == 0);
    // 100 of 10 000 recruit people: 15 townsfolk, 80 peasants, 5 cossacks
    CHECK(size_of(w, 0, Group::Townsfolk) == 1500 - 15);
    CHECK(size_of(w, 0, Group::Peasants) == 8000 - 80);
    CHECK(size_of(w, 0, Group::Cossacks) == 500 - 5);
    CHECK(size_of(w, 1, Group::Peasants) == 8000);  // another province: untouched
    // everything else stays: moods, wealth, control, the other columns of the province
    CHECK(e1.groups.mood == e0.groups.mood);
    CHECK(e1.groups.wealth == e0.groups.wealth);
    CHECK(e1.provinces.control == e0.provinces.control);
    CHECK(e1.provinces.tax_policy == e0.provinces.tax_policy);
    CHECK(e1.accounts == e0.accounts);
    // the cache of the population follows the groups: the sum of the groups is the province population
    CHECK(w.population_persons(ProvinceId(0)) == 10'000 - 100);
    require_valid(w);
}

// ---- the monthly pass ---------------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kStaggered = 29;  // provinces 0..27 have the slot days 1..28; 28 has day 1 again

std::int32_t slot_day(std::uint32_t province) {
    return static_cast<std::int32_t>(province % 28) + 1;
}

}  // namespace

TEST_CASE("the pool regenerates once a month, on the slot day of the province, in months of 28 to 31 days") {
    // 28 (Feb 1703), 29 (Feb 1704 and 1708, Julian leap years, and Feb 1800), 30 (Apr), 31 (Jan)
    const std::array<Date, 7> before{{Date::from_ymd(1702, 12, 31),
                                      Date::from_ymd(1703, 1, 31),
                                      Date::from_ymd(1703, 3, 31),
                                      Date::from_ymd(1704, 1, 31),
                                      Date::from_ymd(1708, 1, 31),
                                      Date::from_ymd(1800, 1, 31),
                                      Date::from_ymd(1800, 12, 31)}};
    std::array<bool, 32> seen{};
    for (const Date start : before) {
        const int length = Date::days_in_month((start + 1).year(), (start + 1).month());
        CAPTURE(length);
        seen[static_cast<std::size_t>(length)] = true;
        Simulation sim = economy_sim(make_world(kStaggered, start));
        std::vector<std::uint32_t> last(kStaggered, 0);
        std::vector<int> changes(kStaggered, 0);
        std::vector<int> changed_on(kStaggered, 0);
        for (int day = 1; day <= length; ++day) {
            sim.run_days(1);
            for (std::uint32_t p = 0; p < kStaggered; ++p) {
                const std::uint32_t now = pool(sim.world(), p);
                if (now != last[p]) {
                    ++changes[p];
                    changed_on[p] = day;
                    CHECK(now > last[p]);
                    CHECK(now <= potential(sim.world(), p));
                }
                last[p] = now;
            }
        }
        for (std::uint32_t p = 0; p < kStaggered; ++p) {
            CAPTURE(p);
            CHECK(changes[p] == 1);  // exactly once in the month, never twice
            CHECK(changed_on[p] == slot_day(p));
        }
        require_valid(sim.world());
    }
    CHECK(seen[28]);
    CHECK(seen[29]);
    CHECK(seen[30]);
    CHECK(seen[31]);
}

TEST_CASE("the pool of a province follows the sizes, the policy and the control month after month") {
    Simulation sim = economy_sim(make_world(kStaggered));
    sim.run_days(40 * 31);
    for (std::uint32_t p = 0; p < kStaggered; ++p) {
        CHECK(pool(sim.world(), p) == potential(sim.world(), p));  // it has arrived
        CHECK(pool(sim.world(), p) > 0);
    }
    // a lighter policy: the next pass cuts the pool to the new potential, never leaves it above
    REQUIRE(sim.submit(Command{DynastyId(0), cmd::SetRecruitPolicy{DynastyId(0), 0}}));
    sim.run_days(1);
    require_valid(sim.world());  // between two slots the pool may be above the cap, never above the ceiling
    sim.run_days(31);
    for (std::uint32_t p = 0; p < kStaggered; ++p) {
        CHECK(pool(sim.world(), p) <= potential(sim.world(), p));
    }
    require_valid(sim.world());
    // a province in turmoil gives nobody
    World world = make_world(1);
    world.economy_columns_mut().recruit_pool[0] = 100;
    world.economy_columns_mut().control[0] = Fixed32::from_int(5);
    CHECK(step_province_recruits(world, rules(), ProvinceId(0)).new_pool == 0);
    CHECK(pool(world, 0) == 0);
    // a world without economy, an unknown province: nothing
    World none(Date::from_ymd(1703, 1, 1), 1);
    CHECK(step_province_recruits(none, rules(), ProvinceId(0)) == RecruitResult{});
    CHECK(step_province_recruits(world, rules(), ProvinceId(9)) == RecruitResult{});
    CHECK(explain_recruit_cap(world, rules(), ProvinceId(0)).root().code == term::kRecruitCap);
}

// ---- ECO-A2 ---------------------------------------------------------------------------------------------

namespace {

/// A province that makes grain, so that the output answers to the people.
World producing_world() {
    World world = make_world(1);
    const EconomyColumnsMut columns = world.economy_columns_mut();
    columns.capacity[kGrainGood] = Fixed32::from_int(100);
    columns.labour_base[0] = 9500;  // peasants + townsfolk at the start
    return world;
}

Kopecks output_value(const World& world) {
    static const StaticPriceSource prices(rules().production);
    return compute_province_production(rules(), production_input(world, prices, ProvinceId(0)))
        .total_value_kop;
}

}  // namespace

TEST_CASE("ECO-A2: a stronger conscription lowers the potential, the peasants, the output and the mood") {
    // the intensified policy of the dynasty, the whole pool taken every month, against the voluntary
    // policy and nobody taken
    World strong = producing_world();
    World gentle = producing_world();
    REQUIRE(strong.set_dynasty_recruit_policy(DynastyId(0), 2));
    REQUIRE(gentle.set_dynasty_recruit_policy(DynastyId(0), 0));
    Simulation hard = economy_sim(std::move(strong));
    Simulation soft = economy_sim(std::move(gentle));

    std::uint64_t conscripted = 0;
    int months_with_draft = 0;
    for (int month = 0; month < 12; ++month) {
        hard.run_days(31);
        soft.run_days(31);
        const World& w = hard.world();
        const std::uint32_t before_pool = pool(w, 0);
        const std::uint32_t before_reserve = reserve(w, 0);
        const std::uint32_t before_potential = potential(w, 0);
        const std::uint32_t before_peasants = size_of(w, 0, Group::Peasants);
        const Kopecks before_output = output_value(w);
        const std::uint64_t before_persons = w.population_persons(ProvinceId(0));
        CHECK(before_pool <= before_potential);  // conscripted <= potential
        if (before_pool == 0) {
            continue;
        }
        const std::uint32_t count = before_pool;
        REQUIRE(hard.submit(conscript(DynastyId(0), 0, count)));
        hard.run_days(1);  // the command is applied at the start of the next tick
        const World& a = hard.world();
        CHECK(reserve(a, 0) == before_reserve + count);  // the reserve grows by exactly the number taken
        conscripted += count;
        ++months_with_draft;
        CHECK(size_of(a, 0, Group::Peasants) < before_peasants);
        CHECK(potential(a, 0) <= before_potential);
        if (count >= 100) {
            CHECK(potential(a, 0) < before_potential);  // a real draft shows in the potential
        }
        CHECK(output_value(a) < before_output);
        CHECK(a.population_persons(ProvinceId(0)) < before_persons);
        // the sum of the groups is the population of the province, always
        std::uint64_t sum = 0;
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            sum += a.economy().groups.size[g];
        }
        CHECK(sum == a.population_persons(ProvinceId(0)));
        CHECK(pool(a, 0) <= potential(a, 0));
        require_valid(a);
    }
    CHECK(months_with_draft >= 6);
    CHECK(reserve(hard.world(), 0) == conscripted);  // and nothing else ever fills the reserve
    CHECK(reserve(soft.world(), 0) == 0);

    // against the gentle province: fewer peasants, less output, a lower mood of the draft groups
    const World& h = hard.world();
    const World& s = soft.world();
    CHECK(size_of(h, 0, Group::Peasants) < size_of(s, 0, Group::Peasants));
    CHECK(output_value(h) < output_value(s));
    CHECK(mood_raw(h, 0, Group::Peasants) < mood_raw(s, 0, Group::Peasants));
    CHECK(mood_raw(h, 0, Group::Cossacks) < mood_raw(s, 0, Group::Cossacks));
    CHECK(mood_raw(h, 0, Group::Townsfolk) < mood_raw(s, 0, Group::Townsfolk));
}

TEST_CASE("conscription never takes more than the potential, whatever the pool holds") {
    World world = make_world(1);
    world.economy_columns_mut().recruit_pool[0] = 400;
    // asking for more than the pool is refused, asking for exactly it works, then it is empty
    CHECK(verdict(world, conscript(DynastyId(0), 0, 401)) == Rejection::InvalidState);
    apply(world, conscript(DynastyId(0), 0, 400));
    CHECK(pool(world, 0) == 0);
    CHECK(reserve(world, 0) == 400);
    CHECK(verdict(world, conscript(DynastyId(0), 0, 1)) == Rejection::InvalidState);
    // the World function itself never exceeds either: a direct call with too much takes what there is
    world.economy_columns_mut().recruit_pool[0] = 50;
    CHECK(world.conscript_recruits(ProvinceId(0), 9999) == 50);
    CHECK(world.conscript_recruits(ProvinceId(0), 9999) == 0);
    CHECK(world.conscript_recruits(ProvinceId(0), 0) == 0);
    CHECK(world.conscript_recruits(ProvinceId(77), 5) == 0);
    CHECK(reserve(world, 0) == 450);
    require_valid(world);
    World crown = make_world(1, Date::from_ymd(1702, 12, 31), true);
    crown.economy_columns_mut().recruit_pool[0] = 10;
    CHECK(crown.conscript_recruits(ProvinceId(0), 5) == 0);  // nobody to hold the reserve
    CHECK(pool(crown, 0) == 10);
}

// ---- replays, saves ---------------------------------------------------------------------------------------

TEST_CASE("a replay (PRP1) with ConscriptRecruits is read back and reproduces the hashes") {
    const auto record = [](Replay* out) {
        Simulation sim = economy_sim(make_world(12));
        ReplayRecorder recorder(5, 77, sim.world().today(), 10);
        for (int day = 0; day < 150; ++day) {
            if (day == 3) {
                REQUIRE(sim.submit(Command{DynastyId(0), cmd::SetRecruitPolicy{DynastyId(0), 2}}));
            }
            if (day == 70 || day == 100) {
                for (std::uint32_t p = 0; p < 12; ++p) {
                    const std::uint32_t ready = pool(sim.world(), p);
                    if (ready > 0) {
                        REQUIRE(sim.submit(conscript(DynastyId(), p, (ready + 1) / 2)));
                    }
                }
            }
            if (day == 110) {
                REQUIRE(sim.submit(conscript(DynastyId(1), 0, 1)));  // refused: not the owner
            }
            (void)sim.tick();
            recorder.after_tick(sim);
        }
        *out = recorder.finish(sim);
        return sim.world().compute_hash();
    };
    Replay replay;
    const WorldHash final_hash = record(&replay);
    CHECK(replay.commands.size() > 1);
    bool has_conscription = false;
    for (const auto& entry : replay.commands) {
        has_conscription = has_conscription || type_of(entry.command) == CommandType::ConscriptRecruits;
    }
    CHECK(has_conscription);

    const std::vector<std::uint8_t> bytes = serialize_replay(replay);
    const auto read = deserialize_replay(bytes);
    REQUIRE(read.has_value());
    CHECK(read.value().commands == replay.commands);
    CHECK(serialize_replay(read.value()) == bytes);

    Simulation player = economy_sim(make_world(12));
    const ReplayResult played = play_replay(player, read.value());
    CHECK(played.error.empty());
    CHECK(played.ok);
    CHECK(player.world().compute_hash() == final_hash);
    CHECK(reserve(player.world(), 0) > 0);

    Replay again;
    CHECK(record(&again) == final_hash);
    CHECK(serialize_replay(again) == bytes);
}

TEST_CASE(
    "a save in the middle of the month keeps the pools and the reserves, and the run goes on identically") {
    for (const bool conscript_before_save : {true, false}) {
        CAPTURE(conscript_before_save);
        World world = make_world(kStaggered);
        Simulation straight = economy_sim(world);
        Simulation first = economy_sim(world);
        straight.run_days(75);  // mid-month, the pools hold people
        first.run_days(75);
        const auto take = [](Simulation& sim) {
            for (std::uint32_t p = 0; p < kStaggered; p += 3) {
                const std::uint32_t ready = pool(sim.world(), p);
                if (ready > 1) {
                    REQUIRE(sim.submit(conscript(DynastyId(0), p, ready - 1)));
                }
            }
        };
        if (conscript_before_save) {
            take(straight);
            take(first);
            straight.run_days(2);
            first.run_days(2);
        }
        pat::core::ByteWriter out;
        first.world().save_state(out);
        pat::core::ByteReader in(out.data());
        auto loaded = World::load_state(in);
        REQUIRE(loaded.has_value());
        CHECK(loaded.value().compute_hash() == first.world().compute_hash());
        CHECK(loaded.value().economy() == first.world().economy());
        Simulation resumed = economy_sim(make_world(kStaggered));
        resumed.restore_state(std::move(loaded).value(), {});
        if (!conscript_before_save) {
            take(straight);
            take(resumed);
        }
        straight.run_days(100);
        resumed.run_days(100);
        CHECK(resumed.world().compute_hash() == straight.world().compute_hash());
        CHECK(resumed.world().economy() == straight.world().economy());
        CHECK(reserve(resumed.world(), 0) > 0);
        require_valid(resumed.world());
    }
}

// ---- the invariant ----------------------------------------------------------------------------------------

TEST_CASE("the rules invariant E-R1 reports a pool above what the people can give") {
    Simulation sim = economy_sim(make_world(3));
    sim.run_days(31);
    require_valid(sim.world());
    World world = sim.world();
    const std::uint32_t ceiling = recruit_ceiling(rules(), {{0, 0, 1500, 8000, 500, 0}});
    // the heaviest level x1.6 of 287 (459), times the best set of buildings x1.25 (the ostrog, E9-T17),
    // rounded up
    CHECK(ceiling == 574);
    // the people of the province now are a little more than at the start: take the ceiling it computes
    std::array<std::uint32_t, kGroupCount> size{};
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        size[g] = world.economy().groups.size[1 * kGroupCount + g];
    }
    const std::uint32_t own = recruit_ceiling(rules(), size);
    world.economy_columns_mut().recruit_pool[1] = own;
    CHECK(check_rules_invariants(world, rules()).empty());
    world.economy_columns_mut().recruit_pool[1] = own + 1;
    const auto bad = check_rules_invariants(world, rules());
    REQUIRE(bad.size() == 1);
    CHECK(bad[0].find("province 1") != std::string::npos);
    CHECK(bad[0].find("recruit pool") != std::string::npos);
    // a province the slot never processed is not checked (a scenario sets its pool)
    World fresh = make_world(2);
    fresh.economy_columns_mut().recruit_pool[0] = 999'999;
    CHECK(check_rules_invariants(fresh, rules()).empty());
}
