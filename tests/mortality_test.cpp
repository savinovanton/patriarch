#include "pat/core/rng.hpp"
#include "pat/sim/mortality.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <variant>

namespace {

using namespace pat::sim;
using pat::core::Fixed32;
using pat::core::RngStreams;

constexpr std::uint64_t kSeed = 42;
constexpr std::string_view kMortalityStream = "demography.mortality";

World dynasty_world(Date start) {
    World world(start, kSeed);
    REQUIRE(world.enable_dynasty_mode());
    return world;
}

Simulation with_mortality(World world, const DemographyRules& rules) {
    Simulation sim(std::move(world));
    sim.add_system(make_mortality_system(rules));
    return sim;
}

CharacterId create_person(World& world, Date birth) {
    CharacterInit init;
    init.birth_date = birth;
    init.health = Fixed32::from_int(50);
    const CharacterId id = world.create_character(init);
    REQUIRE(id.valid());
    return id;
}

DemographyRules zero_mortality() {
    DemographyRules rules = kDefaultDemography;
    for (auto& bucket : rules.mortality.buckets) {
        bucket.value = 0;
    }
    return rules;
}

void check_draw_count(const World& world, std::uint64_t birthdays) {
    RngStreams expected(kSeed);
    expected.stream(kMortalityStream).advance(birthdays * 2);
    CHECK(world.rng().save() == expected.save());
}

Simulation load_mortality_save(const Simulation& original, const DemographyRules& rules) {
    auto loaded = deserialize_save(serialize_save(original.world(), original.pending_internal()));
    if (!loaded) {
        INFO(loaded.error().message);
        REQUIRE(loaded.has_value());
    }
    Simulation resumed = with_mortality(World(original.world().today(), kSeed), rules);
    resumed.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
    return resumed;
}

}  // namespace

TEST_CASE("dynasty.mortality: 108000 annual rolls match the age-bucket probabilities") {
    constexpr std::int64_t kTrialsPerBucket = 12'000;
    const Date birthday = Date::from_ymd(1703, 1, 1);
    for (int i = 0; i < kDefaultDemography.mortality.count; ++i) {
        const AgeBucket bucket = kDefaultDemography.mortality.buckets[static_cast<std::size_t>(i)];
        CAPTURE(bucket.from_age);
        World world = dynasty_world(birthday);
        const Date birth = Date::from_ymd(birthday.year() - bucket.from_age, 1, 1);
        for (std::int64_t person = 0; person < kTrialsPerBucket; ++person) {
            (void)create_person(world, birth);
        }

        Simulation sim = with_mortality(std::move(world), kDefaultDemography);
        const auto& report = sim.tick();
        REQUIRE(report.systems.size() == 1);
        CHECK(report.systems.front().name == kMortalitySystemName);
        CHECK(report.systems.front().wu_used == static_cast<std::uint64_t>(kTrialsPerBucket));
        check_draw_count(sim.world(), static_cast<std::uint64_t>(kTrialsPerBucket));

        // All full-detail people use the child nobility multiplier below the configured age limit.
        const auto expected_permille =
            bucket.from_age < kDefaultDemography.noble_child_age_limit
                ? bucket.value * kDefaultDemography.noble_child_multiplier_permille / 1000
                : bucket.value;
        // Compare with a six-sigma binomial band, in exact integer arithmetic.
        const auto planned = static_cast<std::int64_t>(sim.pending_internal().size());
        const auto deviation = planned * 1000 - kTrialsPerBucket * expected_permille;
        CAPTURE(planned);
        CHECK(deviation * deviation <=
              36 * kTrialsPerBucket * expected_permille * (1000 - expected_permille));

        CharacterId previous;
        for (const Command& command : sim.pending_internal()) {
            const auto* plan = std::get_if<cmd::PlanDeath>(&command.payload);
            REQUIRE(plan != nullptr);
            REQUIRE(plan->date >= birthday);
            REQUIRE(plan->date < birthday + 365);
            REQUIRE((!previous.valid() || previous.value() < plan->character.value()));
            previous = plan->character;
        }
    }
}

TEST_CASE("dynasty.mortality: birth uses age zero and a pending plan survives a save") {
    const Date birth = Date::from_ymd(1703, 1, 2);
    World world = dynasty_world(birth - 1);
    const DynastyId dynasty = world.create_dynasty({});
    DemographyRules rules = zero_mortality();
    rules.mortality.buckets[0].value = 1000;
    rules.noble_child_multiplier_permille = 1000;
    Simulation original = with_mortality(std::move(world), rules);

    CharacterInit init;
    init.dynasty = dynasty;
    init.birth_date = birth;
    init.health = Fixed32::from_int(50);
    REQUIRE(original.submit({{}, cmd::CreatePerson{init}}));
    CHECK(original.tick().external_commands.applied == 1);
    REQUIRE(original.pending_internal().size() == 1);
    const auto* plan = std::get_if<cmd::PlanDeath>(&original.pending_internal().front().payload);
    REQUIRE(plan != nullptr);
    const CharacterId baby = plan->character;
    const Date death_due = plan->date;
    CHECK(original.world().characters().birth_date[baby.index()] == birth);
    CHECK(original.world().characters().planned_death[baby.index()] == kNoDate);
    check_draw_count(original.world(), 1);

    auto resumed = load_mortality_save(original, rules);
    REQUIRE(std::ranges::equal(original.pending_internal(), resumed.pending_internal()));
    const Date dispatch = std::max(death_due, birth + 1);
    const int days_until_dispatch = dispatch - birth;
    original.run_days(days_until_dispatch);
    resumed.run_days(days_until_dispatch);
    CHECK(original.world().compute_hash() == resumed.world().compute_hash());
    CHECK(original.world().characters().planned_death[baby.index()] == death_due);
    CHECK(original.world().check_invariants().empty());
    REQUIRE(original.pending_internal().size() == 1);
    REQUIRE(std::holds_alternative<cmd::Die>(original.pending_internal().front().payload));
    REQUIRE(std::ranges::equal(original.pending_internal(), resumed.pending_internal()));

    auto resumed_death = load_mortality_save(original, rules);
    CHECK(original.tick().internal_commands.applied == 1);
    CHECK(resumed.tick().internal_commands.applied == 1);
    CHECK(resumed_death.tick().internal_commands.applied == 1);
    CHECK(original.world().compute_hash() == resumed.world().compute_hash());
    CHECK(original.world().compute_hash() == resumed_death.world().compute_hash());
    const auto archived = original.world().archived_person(baby);
    REQUIRE(archived.has_value());
    CHECK(archived->death_date == dispatch + 1);
    CHECK(archived->cause == CauseOfDeath::Illness);
    CHECK(original.world().check_invariants().empty());
}

TEST_CASE("dynasty.mortality: a leap-day birthday is processed once per year across an initial save") {
    const Date birth = Date::from_ymd(1704, 2, 29);
    World world = dynasty_world(birth);
    const auto child = create_person(world, birth);
    const DemographyRules rules = zero_mortality();
    Simulation original = with_mortality(std::move(world), rules);
    auto resumed = load_mortality_save(original, rules);

    CHECK(original.tick().wu_used == 1);
    CHECK(resumed.tick().wu_used == 1);
    check_draw_count(original.world(), 1);
    CHECK(original.world().compute_hash() == resumed.world().compute_hash());

    const Date common_anniversary = Date::from_ymd(1705, 2, 28);
    const int days_until_anniversary = common_anniversary - original.world().today();
    original.run_days(days_until_anniversary - 1);
    resumed.run_days(days_until_anniversary - 1);
    check_draw_count(original.world(), 1);
    CHECK(original.tick().wu_used == 1);
    CHECK(resumed.tick().wu_used == 1);
    CHECK(original.world().today() == common_anniversary);
    check_draw_count(original.world(), 2);
    CHECK(original.tick().wu_used == 0);
    CHECK(resumed.tick().wu_used == 0);
    check_draw_count(original.world(), 2);
    CHECK(original.world().compute_hash() == resumed.world().compute_hash());
    CHECK(original.world().is_alive(child));
}

TEST_CASE("dynasty.mortality: only the current planned death emits a command") {
    const Date start = Date::from_ymd(1703, 1, 1);
    World world = dynasty_world(start);
    const auto person = create_person(world, Date::from_ymd(1620, 1, 2));
    REQUIRE(world.set_character_planned_death(person, start + 2));
    REQUIRE(world.set_character_planned_death(person, start + 1));
    REQUIRE(world.set_character_planned_death(person, start + 2));
    const auto treated = create_person(world, Date::from_ymd(1680, 7, 1));
    REQUIRE(world.set_character_planned_death(treated, start + 2));
    const Command cancel{{}, cmd::PlanDeath{treated, kNoDate}};
    REQUIRE(validate(world, cancel).has_value());
    apply(world, cancel);

    DemographyRules rules = zero_mortality();
    rules.mortality.buckets[8].value = 1000;
    Simulation sim = with_mortality(std::move(world), rules);

    // The birthday still draws twice, but it cannot overwrite a death that was already planned.
    CHECK(sim.tick().wu_used == 1);
    check_draw_count(sim.world(), 1);
    CHECK(sim.pending_internal().empty());
    CHECK(sim.tick().wu_used == 1);
    REQUIRE(sim.pending_internal().size() == 1);
    const auto* death = std::get_if<cmd::Die>(&sim.pending_internal().front().payload);
    REQUIRE(death != nullptr);
    CHECK(death->character == person);
    CHECK(death->cause == CauseOfDeath::OldAge);
    CHECK(sim.tick().internal_commands.applied == 1);
    CHECK(sim.world().is_archived(person));
    CHECK(sim.world().is_alive(treated));
    CHECK(sim.pending_internal().empty());
}
