#include "pat/core/rng.hpp"
#include "pat/sim/health.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace pat::sim;
using pat::core::Fixed32;
using pat::core::RngStreams;

constexpr std::uint64_t kSeed = 77;
constexpr std::uint32_t kLowRoll = 0;
constexpr std::uint32_t kHighRoll = 0xFFFFFFFFU;

Fixed32 points(int value) {
    return Fixed32::from_int(value);
}

World dynasty_world(Date start) {
    World world(start, kSeed);
    REQUIRE(world.enable_dynasty_mode());
    return world;
}

CharacterId person(World& world, int age, int health = 70, DynastyId dynasty = {}) {
    CharacterInit init;
    init.dynasty = dynasty;
    init.birth_date = Date::from_ymd(world.today().year() - age, 1, 1);
    init.health = points(health);
    const CharacterId id = world.create_character(init);
    REQUIRE(id.valid());
    return id;
}

Simulation
with_health(World world, const DemographyRules& rules, std::shared_ptr<const TraitCatalog> traits = {}) {
    Simulation sim(std::move(world));
    sim.add_system(make_health_system(rules, std::move(traits)));
    return sim;
}

/// Rules with no randomness and no feedback: only the drift of the stages.
DemographyRules plain_rules() {
    DemographyRules rules = kDefaultDemography;
    rules.health_noise = 0;
    rules.low_health_drift = 0;
    rules.stress_high_health_drift = 0;
    rules.stress_critical_health_drift = 0;
    rules.stress_gain_low_health = 0;
    rules.stress_gain_head = 0;
    rules.stress_decay_permille = 0;
    return rules;
}

void run_days(Simulation& sim, int days) {
    for (int i = 0; i < days; ++i) {
        (void)sim.tick();
    }
}

std::shared_ptr<const TraitCatalog> catalog_with(std::uint16_t id, CharacterAttribute attribute, int value) {
    auto catalog = std::make_shared<TraitCatalog>();
    TraitDef def;
    def.id = id;
    def.key = "test_trait";
    def.name_key = "trait.test.name";
    def.desc_key = "trait.test.desc";
    def.attribute_modifiers[static_cast<std::size_t>(attribute)] = points(value);
    catalog->definitions.push_back(def);
    REQUIRE(validate(*catalog).has_value());
    return catalog;
}

}  // namespace

// ---- pure functions ------------------------------------------------------------------------------

TEST_CASE("monthly_health_change: the drift follows the life stage and the terms add up") {
    const DemographyRules rules = plain_rules();
    const auto change_at = [&](int age) {
        const VitalsChange change = monthly_health_change(rules, age, points(70), points(0));
        CHECK(change.sum_of_terms() == change.total);
        return change.total;
    };
    CHECK(change_at(3) == Fixed32::from_ratio(15, 100));
    CHECK(change_at(10) == Fixed32::from_ratio(10, 100));
    CHECK(change_at(30) == Fixed32::zero());
    CHECK(change_at(55) == Fixed32::from_ratio(-10, 100));
    CHECK(change_at(70) == Fixed32::from_ratio(-40, 100));
    CHECK(change_at(70) < change_at(30));
    // The stage boundaries are those of the rules.
    CHECK(change_at(rules.mature_from - 1) == Fixed32::zero());
    CHECK(change_at(rules.mature_from) == Fixed32::from_ratio(-10, 100));
    CHECK(change_at(rules.elder_from) == Fixed32::from_ratio(-40, 100));
}

TEST_CASE("monthly_health_change: low health, stress levels and the swing") {
    DemographyRules rules = kDefaultDemography;
    rules.health_noise = 0;

    const VitalsChange base = monthly_health_change(rules, 30, points(70), points(0));
    CHECK(base.terms.size() == 0);  // a zero drift records nothing
    CHECK(base.total == Fixed32::zero());

    const VitalsChange sick = monthly_health_change(rules, 30, points(29), points(0));
    CHECK(sick.total == Fixed32::from_ratio(rules.low_health_drift, 100));
    CHECK(sick.terms.size() == 1);
    CHECK(sick.terms[0].key == "demography.health.low_health");

    const VitalsChange strained = monthly_health_change(rules, 30, points(70), points(60));
    CHECK(strained.total == Fixed32::from_ratio(rules.stress_high_health_drift, 100));
    // The breakdown replaces the strain penalty, it does not add to it.
    const VitalsChange breaking = monthly_health_change(rules, 30, points(70), points(90));
    CHECK(breaking.total == Fixed32::from_ratio(rules.stress_critical_health_drift, 100));
    CHECK(breaking.terms.size() == 1);
    CHECK(breaking.sum_of_terms() == breaking.total);

    // The swing: the extremes of the roll give the extremes of the range, the middle gives none.
    rules.health_noise = 20;
    CHECK(monthly_health_change(rules, 30, points(70), points(0), kLowRoll).total ==
          Fixed32::from_ratio(-20, 100));
    CHECK(monthly_health_change(rules, 30, points(70), points(0), kHighRoll).total ==
          Fixed32::from_ratio(20, 100));
    CHECK(monthly_health_change(rules, 30, points(70), points(0), kNeutralHealthRoll).total ==
          Fixed32::zero());
}

TEST_CASE("monthly_stress_change: gains, decay and the bounds") {
    DemographyRules rules = kDefaultDemography;
    rules.stress_decay_permille = 100;

    const VitalsChange calm = monthly_stress_change(rules, points(50), points(70), false);
    CHECK(calm.total == points(50).mul_permille(900) - points(50));  // a tenth fades
    CHECK(calm.sum_of_terms() == calm.total);

    const VitalsChange ill_head = monthly_stress_change(rules, points(10), points(10), true);
    CHECK(ill_head.terms.size() == 3);
    CHECK(ill_head.sum_of_terms() == ill_head.total);
    CHECK(points(10) + ill_head.total ==
          (points(10) + Fixed32::from_ratio(rules.stress_gain_low_health, 100) +
           Fixed32::from_ratio(rules.stress_gain_head, 100))
              .mul_permille(900));

    // Never above 100, never below 0.
    rules.stress_decay_permille = 0;
    const VitalsChange full = monthly_stress_change(rules, points(100), points(10), true);
    CHECK(points(100) + full.total == points(100));
    CHECK(full.terms.back().key == "demography.stress.clamp");
    rules.stress_gain_low_health = 0;
    rules.stress_gain_head = 0;
    CHECK(monthly_stress_change(rules, points(0), points(70), false).total == Fixed32::zero());
}

TEST_CASE("stress_level and derived_fertility") {
    const DemographyRules rules = kDefaultDemography;
    CHECK(stress_level(rules, points(0)) == StressLevel::Calm);
    CHECK(stress_level(rules, Fixed32::from_ratio(599, 10)) == StressLevel::Calm);
    CHECK(stress_level(rules, points(60)) == StressLevel::Strained);
    CHECK(stress_level(rules, Fixed32::from_ratio(899, 10)) == StressLevel::Strained);
    CHECK(stress_level(rules, points(90)) == StressLevel::Breaking);
    CHECK(stress_level(rules, points(100)) == StressLevel::Breaking);

    CHECK(derived_fertility(rules, 5, points(70)) == Fixed32::zero());
    CHECK(derived_fertility(rules, 17, points(70)) == points(80));
    CHECK(derived_fertility(rules, 25, points(70)) == points(100));
    CHECK(derived_fertility(rules, 25, points(20)) == points(50));   // an unwell body halves it
    CHECK(derived_fertility(rules, 25, points(90)) == points(100));  // cut to the scale
    CHECK(derived_fertility(rules, 37, points(70)) == points(50));
    CHECK(derived_fertility(rules, 42, points(70)) == points(20));
    CHECK(derived_fertility(rules, 50, points(70)) == Fixed32::zero());
}

// ---- the system ----------------------------------------------------------------------------------

TEST_CASE("dynasty.health: an old man loses health faster than a man in his prime") {
    World world = dynasty_world(Date::from_ymd(1703, 1, 1));
    std::vector<CharacterId> old_people;
    std::vector<CharacterId> prime_people;
    for (int i = 0; i < 200; ++i) {
        old_people.push_back(person(world, 70));
        prime_people.push_back(person(world, 30));
    }
    Simulation sim = with_health(std::move(world), kDefaultDemography);
    run_days(sim, 365);

    std::int64_t old_sum = 0;
    std::int64_t prime_sum = 0;
    for (const CharacterId id : old_people) {
        old_sum += sim.world().characters().health[id.index()].raw();
    }
    for (const CharacterId id : prime_people) {
        prime_sum += sim.world().characters().health[id.index()].raw();
    }
    const std::int64_t one_point = Fixed32::one_raw;
    CHECK(old_sum / 200 < 70 * one_point - 3 * one_point);  // about -4.8 a year
    CHECK(prime_sum / 200 > 70 * one_point - one_point);    // about 0
    CHECK(old_sum < prime_sum);
}

TEST_CASE("dynasty.health: the bounds 0...100 hold whatever the start") {
    DemographyRules rules = kDefaultDemography;
    rules.health_noise = 5000;  // violent swings
    rules.stress_gain_head = 5000;
    World world = dynasty_world(Date::from_ymd(1703, 1, 1));
    const DynastyId dynasty = world.create_dynasty({});
    std::vector<CharacterId> people;
    for (int age : {2, 20, 45, 60, 80, 95}) {
        for (int health : {0, 1, 50, 99, 100}) {
            people.push_back(person(world, age, health, dynasty));
        }
    }
    REQUIRE(world.set_dynasty_head(dynasty, people[3]));
    for (const CharacterId id : people) {
        world.set_character_stress(id, points(95));
    }
    Simulation sim = with_health(std::move(world), rules);
    for (int day = 0; day < 4 * 365; ++day) {
        (void)sim.tick();
        for (const CharacterId id : people) {
            const CharacterTable& table = sim.world().characters();
            REQUIRE(table.health[id.index()] >= Fixed32::zero());
            REQUIRE(table.health[id.index()] <= points(100));
            REQUIRE(table.stress[id.index()] >= Fixed32::zero());
            REQUIRE(table.stress[id.index()] <= points(100));
            REQUIRE(table.fertility[id.index()] >= Fixed32::zero());
            REQUIRE(table.fertility[id.index()] <= points(100));
        }
    }
    CHECK(sim.world().check_invariants().empty());
}

TEST_CASE("dynasty.health: one draw and one unit of work per character, only on the monthly slot") {
    World world = dynasty_world(Date::from_ymd(1703, 1, 1));
    constexpr int kPeople = 100;
    for (int i = 0; i < kPeople; ++i) {
        (void)person(world, 20 + i % 40);
    }
    Simulation sim = with_health(std::move(world), kDefaultDemography);

    std::uint64_t draws = 0;
    for (int day = 0; day < 120; ++day) {
        const auto& report = sim.tick();
        const Date today = sim.world().today();
        std::uint64_t expected = 0;
        for (const CharacterId id : sim.world().living()) {
            expected += is_monthly_slot(id.value(), today) ? 1 : 0;
        }
        CAPTURE(today.days());
        CHECK(report.systems.front().name == kHealthSystemName);
        CHECK(report.systems.front().wu_used == expected);
        if (today.day() > 28) {
            CHECK(expected == 0);
        }
        draws += expected;
    }
    // Every character was visited once a month: kPeople per month for about four months.
    CHECK(draws >= 3 * kPeople);
    CHECK(draws <= 5 * kPeople);
    RngStreams expected_streams(kSeed);
    expected_streams.stream(kHealthStream).advance(draws);
    CHECK(sim.world().rng().save() == expected_streams.save());
}

TEST_CASE("dynasty.health: nothing happens outside dynasty mode") {
    World world(Date::from_ymd(1703, 1, 1), kSeed);
    CharacterInit init;
    init.birth_date = Date::from_ymd(1650, 1, 1);
    init.health = points(60);
    const CharacterId id = world.create_character(init);
    Simulation sim = with_health(std::move(world), kDefaultDemography);
    run_days(sim, 90);
    CHECK(sim.world().characters().health[id.index()] == points(60));
    CHECK(sim.world().rng().save() == RngStreams(kSeed).save());
}

TEST_CASE("dynasty.health: fertility is derived from the age and the health of the month") {
    World world = dynasty_world(Date::from_ymd(1703, 1, 1));
    const CharacterId young = person(world, 25, 70);
    const CharacterId middle = person(world, 42, 70);
    const CharacterId child = person(world, 5, 70);
    const CharacterId sick = person(world, 25, 10);
    Simulation sim = with_health(std::move(world), plain_rules());
    run_days(sim, 40);
    const CharacterTable& table = sim.world().characters();
    CHECK(table.fertility[young.index()] == points(100));
    CHECK(table.fertility[middle.index()] == points(20));
    CHECK(table.fertility[child.index()] == Fixed32::zero());
    CHECK(table.fertility[sick.index()] == points(50));
}

TEST_CASE("dynasty.health: traits count through the effective health and stress") {
    // `sickly`-like trait: -15 health. A stored health of 40 is effectively 25 (< 30): illness worries.
    const auto sickly = catalog_with(5, CharacterAttribute::Health, -15);
    DemographyRules rules = plain_rules();
    rules.stress_gain_low_health = 100;

    World world = dynasty_world(Date::from_ymd(1703, 1, 1));
    const CharacterId with_trait = person(world, 30, 40);
    const CharacterId without = person(world, 30, 40);
    const std::array<std::uint16_t, 1> trait{5};
    REQUIRE(world.set_character_traits(with_trait, trait));
    Simulation sim = with_health(std::move(world), rules, sickly);
    run_days(sim, 40);
    const CharacterTable& table = sim.world().characters();
    CHECK(table.stress[with_trait.index()] == Fixed32::from_ratio(100, 100));
    CHECK(table.stress[without.index()] == Fixed32::zero());
    // The stored health is the base column: the trait is not baked into it.
    CHECK(table.health[with_trait.index()] == points(40));
}

TEST_CASE("dynasty.health: a rise to a higher stress level is written to the chronicle once") {
    DemographyRules rules = plain_rules();
    rules.stress_gain_head = 100;
    World world = dynasty_world(Date::from_ymd(1703, 1, 1));
    const DynastyId dynasty = world.create_dynasty({});
    const CharacterId head = person(world, 40, 70, dynasty);
    const CharacterId other = person(world, 40, 70, dynasty);
    REQUIRE(world.set_dynasty_head(dynasty, head));
    world.set_character_stress(head, Fixed32::from_ratio(595, 10));  // 59.5 -> 60.5: strained
    world.set_character_stress(other, Fixed32::from_ratio(595, 10));
    Simulation sim = with_health(std::move(world), rules);
    run_days(sim, 40);  // the slot of each of them has come once

    const auto notes = sim.world().query_chronicle(
        dynasty, Date::from_ymd(1703, 1, 1), Date::from_ymd(1703, 12, 31), ChronicleKind::Note);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].a == head);
    CHECK(notes[0].param == kNoteStressStrained);

    // Staying at the same level writes nothing more; the next level does. 100 a month is a point a month.
    run_days(sim, 28 * 40);
    std::int32_t strained = 0;
    std::int32_t breaking = 0;
    for (const auto& note : sim.world().chronicle(dynasty)) {
        if (note.kind == ChronicleKind::Note) {
            strained += note.param == kNoteStressStrained ? 1 : 0;
            breaking += note.param == kNoteStressBreaking ? 1 : 0;
        }
    }
    CHECK(strained == 1);
    CHECK(breaking == 1);
}

TEST_CASE("dynasty.health: two runs agree, and a save in the middle changes nothing") {
    const auto build = [] {
        World world = dynasty_world(Date::from_ymd(1703, 1, 1));
        const DynastyId dynasty = world.create_dynasty({});
        for (int i = 0; i < 60; ++i) {
            (void)person(world, 1 + i * 3 % 90, 20 + i * 7 % 80, dynasty);
        }
        return world;
    };
    Simulation a = with_health(build(), kDefaultDemography);
    Simulation b = with_health(build(), kDefaultDemography);
    run_days(a, 400);
    run_days(b, 400);
    CHECK(a.world().compute_hash() == b.world().compute_hash());

    Simulation first = with_health(build(), kDefaultDemography);
    run_days(first, 150);
    auto loaded = deserialize_save(serialize_save(first.world(), first.pending_internal()));
    REQUIRE(loaded.has_value());
    Simulation resumed = with_health(World(first.world().today(), kSeed), kDefaultDemography);
    resumed.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
    run_days(resumed, 250);
    CHECK(resumed.world().compute_hash() == a.world().compute_hash());
}
