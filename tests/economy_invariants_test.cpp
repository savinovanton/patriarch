#include "pat/sim/calendar.hpp"
#include "pat/sim/economy/invariants.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// Level R of the economy invariants (E9 #243, docs/dev/economy-model.md section 12): the checks that need
// the rules. The damage is written around the API on purpose, as in economy_world_test.cpp.

using namespace pat::sim;
using namespace pat::sim::economy;
using pat::core::Fixed32;
using pat::core::StringId;

namespace {

const EconomyRules& rules() {
    static const EconomyRules value = EconomyRules::defaults();
    return value;
}

EconomyState& state(World& world) {
    return const_cast<EconomyState&>(world.economy());
}

World make_world(std::uint32_t provinces = 3) {
    World world(Date::from_ymd(1702, 12, 31), 5);
    for (std::uint32_t d = 0; d < 2; ++d) {
        (void)world.create_dynasty(StringId(1 + d));
    }
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.create_province(StringId(100 + p), DynastyId(p % 2), Fixed32::from_int(1));
    }
    REQUIRE(world.enable_economy());
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.set_group_size(ProvinceId(p), Group::Peasants, 1000);
        world.economy_columns_mut().tax_policy[p] = static_cast<std::uint8_t>(rules().tax.default_level);
        world.economy_columns_mut().recruit_policy[p] =
            static_cast<std::uint8_t>(rules().recruit.default_level);
    }
    return world;
}

bool mentions(const std::vector<std::string>& problems, const std::string& needle) {
    return std::any_of(problems.begin(), problems.end(), [&](const std::string& p) {
        return p.find(needle) != std::string::npos;
    });
}

}  // namespace

TEST_CASE("rules invariants: a consistent world and a world without economy pass") {
    World world = make_world();
    CHECK(check_rules_invariants(world, rules()).empty());
    CHECK(world.check_invariants().empty());

    const World plain(Date::from_ymd(1703, 1, 1), 1);
    CHECK(check_rules_invariants(plain, rules()).empty());
}

TEST_CASE("rules invariants E-R1: a policy level the rules do not define is reported") {
    World world = make_world();
    world.economy_columns_mut().tax_policy[1] = 200;
    // the world alone cannot know: level K lets it through, level R does not
    CHECK(world.check_invariants().empty());
    const auto problems = check_rules_invariants(world, rules());
    CHECK(mentions(problems, "province 1 has the tax policy level 200"));
    CHECK(problems.size() == 1);

    world.economy_columns_mut().tax_policy[1] = static_cast<std::uint8_t>(rules().tax.level_count - 1);
    CHECK(check_rules_invariants(world, rules()).empty());
    world.economy_columns_mut().tax_policy[1] = static_cast<std::uint8_t>(rules().tax.level_count);
    CHECK_FALSE(check_rules_invariants(world, rules()).empty());

    world.economy_columns_mut().tax_policy[1] = 0;
    world.economy_columns_mut().recruit_policy[2] = 99;
    CHECK(mentions(check_rules_invariants(world, rules()), "province 2 has the recruit policy level 99"));
}

TEST_CASE("rules invariants E-R1: a building type the rules do not define is reported") {
    World world = make_world();
    world.economy_columns_mut().slots_total[0] = 3;
    REQUIRE(world.set_slot_building(ProvinceId(0), 0, rules().buildings.defs[0].slot));
    CHECK(check_rules_invariants(world, rules()).empty());

    REQUIRE(world.set_slot_building(ProvinceId(0), 1, 255));
    CHECK(world.check_invariants().empty());  // K does not know the rules
    const auto problems = check_rules_invariants(world, rules());
    CHECK(mentions(problems, "province 0 slot 1 holds the building 255"));
    CHECK(problems.size() == 1);

    // a world that was saved with richer rules: the same world is fine under them and not under the defaults
    EconomyRules richer = rules();
    REQUIRE(richer.buildings.count < static_cast<std::int32_t>(kMaxBuildingTypes));
    richer.buildings.defs[static_cast<std::size_t>(richer.buildings.count)] = rules().buildings.defs[0];
    richer.buildings.defs[static_cast<std::size_t>(richer.buildings.count)].slot = 255;
    ++richer.buildings.count;
    CHECK(check_rules_invariants(world, richer).empty());

    // an unbuilt slot (below slots_total) with 0 is fine; a building past slots_total is E-B1 of level K
    REQUIRE(world.set_slot_building(ProvinceId(0), 1, 0));
    CHECK(check_rules_invariants(world, rules()).empty());
}

TEST_CASE("rules invariants E-R1: the construction queue builds only known buildings") {
    World world = make_world();
    EconomyState& e = state(world);
    e.provinces.slots_total[1] = 3;
    e.meta.next_construction_seq = 1;
    ConstructionQueue& q = e.construction;
    q.province.push_back(ProvinceId(1));
    q.slot.push_back(0);
    q.building.push_back(rules().buildings.defs[0].slot);
    q.payer.push_back(DynastyId(1));
    q.start_date.push_back(Date::from_ymd(1703, 1, 1));
    q.finish_date.push_back(Date::from_ymd(1703, 6, 1));
    q.paid_kop.push_back(4'000);
    q.seq.push_back(0);
    CHECK(world.check_invariants().empty());
    CHECK(check_rules_invariants(world, rules()).empty());

    q.building[0] = 250;
    CHECK(world.check_invariants().empty());
    CHECK(mentions(check_rules_invariants(world, rules()), "construction entry 0 builds the building 250"));
}

TEST_CASE("rules invariants E-R1: grain above the cap of a processed province is reported") {
    World world = make_world(2);
    // never processed: a scenario sets its stock freely
    world.economy_columns_mut().grain_stock[0] = Fixed32::from_int(30'000);
    CHECK(check_rules_invariants(world, rules()).empty());

    // processed by the slot, then above the cap: reported
    world.economy_columns_mut().last_slot_date[0] = Date::from_ymd(1703, 1, 1);
    const auto problems = check_rules_invariants(world, rules());
    CHECK(mentions(problems, "province 0 holds"));
    CHECK(mentions(problems, "above the cap"));

    // a run of the real system never breaks it
    World run = make_world(8);
    for (std::uint32_t p = 0; p < 8; ++p) {
        run.economy_columns_mut().grain_stock[p] = Fixed32::from_int(500);
        run.economy_columns_mut().capacity[p * kMaxGoods + kGrainGood] = Fixed32::from_int(200);
    }
    Simulation sim(std::move(run));
    install_economy_systems(sim, rules());
    for (int month = 0; month < 36; ++month) {
        sim.run_days(30);
        const auto after = check_rules_invariants(sim.world(), rules());
        for (const auto& problem : after) {
            MESSAGE(problem);
        }
        REQUIRE(after.empty());
    }
}

TEST_CASE("rules invariants E-L1: more loans than the rules allow are reported") {
    World world = make_world();
    EconomyState& e = state(world);
    const std::int32_t max = rules().loans.max_loans_per_dynasty;
    REQUIRE(max >= 1);
    for (std::int32_t i = 0; i < max + 1; ++i) {
        e.loans.loan_id.push_back(static_cast<std::uint32_t>(i));
        e.loans.borrower.push_back(DynastyId(1));
        e.loans.principal.push_back(1000);
        e.loans.rate_ppm_year.push_back(rules().loans.rate_ppm_year);
        e.loans.issued.push_back(Date::from_ymd(1703, 1, 1));
        e.loans.due.push_back(Date::from_ymd(1704, 1, 1));
        e.loans.interest_carry.push_back(0);
        e.loans.overdue.push_back(0);
        e.loans.collateral.push_back(ProvinceId());
    }
    e.meta.next_loan_id = static_cast<std::uint32_t>(max + 1);
    CHECK(world.check_invariants().empty());
    const auto problems = check_rules_invariants(world, rules());
    CHECK(mentions(problems, "dynasty 1 holds"));
    CHECK(problems.size() == 1);

    EconomyRules lenient = rules();
    lenient.loans.max_loans_per_dynasty = max + 1;
    CHECK(check_rules_invariants(world, lenient).empty());
}
