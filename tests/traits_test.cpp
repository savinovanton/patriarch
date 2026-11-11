#include "pat/core/rng.hpp"
#include "pat/sim/mortality.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/traits.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace pat::sim;
using pat::core::Fixed32;
using pat::core::StringId;

namespace {

Fixed32 fx(int value) {
    return Fixed32::from_int(value);
}

TraitDef trait(std::uint16_t id) {
    TraitDef def;
    def.id = id;
    def.key = "trait_" + std::to_string(id);
    def.name_key = def.key + ".name";
    def.desc_key = def.key + ".desc";
    return def;
}

TraitCatalog plain_catalog(std::uint16_t count) {
    TraitCatalog catalog;
    for (std::uint16_t id = 1; id <= count; ++id) {
        catalog.definitions.push_back(trait(id));
    }
    return catalog;
}

struct Person {
    World world{Date::from_ymd(1703, 1, 1), 7};
    CharacterId id;

    explicit Person(int health = 80) {
        REQUIRE(world.enable_dynasty_mode());
        CharacterInit init;
        init.dynasty = world.create_dynasty(StringId(0));
        init.birth_date = Date::from_ymd(1670, 1, 1);
        init.health = fx(health);
        init.skills.fill(fx(10));
        init.fertility = fx(40);
        init.stress = fx(5);
        init.piety = fx(50);
        init.ambition = fx(-80);
        init.loyalty_head = fx(30);
        init.loyalty_emperor = fx(-30);
        id = world.create_character(init);
        REQUIRE(id.valid());
    }
};

bool has_kind(const CharacterBreakdown& breakdown, CharacterTermKind kind) {
    return std::any_of(breakdown.terms.begin(), breakdown.terms.end(), [kind](const CharacterTerm& term) {
        return term.kind == kind;
    });
}

class TraitEmitter final : public System {
public:
    [[nodiscard]] std::string_view name() const override { return "test.traits"; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }
    void run(World&, TickContext& context) override {
        if (context.tick_index == 1) {
            context.emit(Command{DynastyId(), cmd::GrantTrait{CharacterId(0), 1}});
        } else if (context.tick_index == 3) {
            context.emit(Command{DynastyId(), cmd::RevokeTrait{CharacterId(0), 1}});
        }
    }
};

Simulation trait_simulation(const std::shared_ptr<const TraitCatalog>& catalog) {
    Person person;
    SimulationConfig config;
    config.traits = catalog;
    Simulation simulation(std::move(person.world), config);
    simulation.add_system(std::make_unique<TraitEmitter>());
    return simulation;
}

}  // namespace

TEST_CASE("traits: catalog rejects invalid ids, references and numeric domains") {
    auto catalog = plain_catalog(2);
    REQUIRE(validate(catalog).has_value());
    REQUIRE(catalog.find(1) != nullptr);
    CHECK(catalog.find(1)->key == "trait_1");
    CHECK(catalog.find(0) == nullptr);
    CHECK(catalog.find(3) == nullptr);

    SUBCASE("ids are nonzero and strictly increasing") {
        catalog.definitions[0].id = 0;
        CHECK_FALSE(validate(catalog).has_value());
        catalog.definitions[0].id = 2;
        CHECK_FALSE(validate(catalog).has_value());
        catalog.definitions[1].id = 1;
        CHECK_FALSE(validate(catalog).has_value());
    }
    SUBCASE("opposite refers to another known trait") {
        catalog.definitions[0].opposite = 3;
        CHECK_FALSE(validate(catalog).has_value());
        catalog.definitions[0].opposite = 1;
        CHECK_FALSE(validate(catalog).has_value());
    }
    SUBCASE("inheritance is a probability") {
        catalog.definitions[0].inherit_chance = Fixed32::from_raw(-1);
        CHECK_FALSE(validate(catalog).has_value());
        catalog.definitions[0].inherit_chance = Fixed32::one() + Fixed32::from_raw(1);
        CHECK_FALSE(validate(catalog).has_value());
    }
    SUBCASE("multipliers have bounded nonnegative values") {
        catalog.definitions[0].health_multiplier_permille = -1;
        CHECK_FALSE(validate(catalog).has_value());
        catalog.definitions[0].health_multiplier_permille = 1000;
        catalog.definitions[0].fertility_multiplier_permille = 10001;
        CHECK_FALSE(validate(catalog).has_value());
        catalog.definitions[0].fertility_multiplier_permille = 1000;
        catalog.definitions[0].mortality_multiplier_permille = 10001;
        CHECK_FALSE(validate(catalog).has_value());
    }
    SUBCASE("additive modifiers respect the supported domains") {
        catalog.definitions[0].skill_modifiers[0] = fx(11);
        CHECK_FALSE(validate(catalog).has_value());
        catalog.definitions[0].skill_modifiers[0] = fx(10);
        catalog.definitions[0].attribute_modifiers[0] = fx(-101);
        CHECK_FALSE(validate(catalog).has_value());
    }
}

TEST_CASE("traits: grant and revoke keep sorted slots and refuse without mutation") {
    Person person;
    const auto catalog = plain_catalog(9);
    const auto initial_hash = person.world.compute_hash();
    CHECK(check_grant_trait(person.world, catalog, person.id, 3) == TraitStatus::Ok);
    CHECK(person.world.compute_hash() == initial_hash);
    REQUIRE(grant_trait(person.world, catalog, person.id, 3) == TraitStatus::Ok);
    REQUIRE(grant_trait(person.world, catalog, person.id, 1) == TraitStatus::Ok);
    CHECK(person.world.characters().traits[person.id.index()] ==
          std::array<std::uint16_t, kTraitSlots>{1, 3, 0, 0, 0, 0, 0, 0});

    const auto before_refusals = person.world.compute_hash();
    CHECK(check_revoke_trait(person.world, catalog, person.id, 1) == TraitStatus::Ok);
    CHECK(grant_trait(person.world, catalog, person.id, 1) == TraitStatus::AlreadyPresent);
    CHECK(grant_trait(person.world, catalog, person.id, 0) == TraitStatus::Unknown);
    CHECK(grant_trait(person.world, catalog, person.id, 10) == TraitStatus::Unknown);
    CHECK(revoke_trait(person.world, catalog, person.id, 2) == TraitStatus::Missing);
    CHECK(revoke_trait(person.world, catalog, person.id, 10) == TraitStatus::Unknown);
    CHECK(person.world.compute_hash() == before_refusals);

    REQUIRE(revoke_trait(person.world, catalog, person.id, 1) == TraitStatus::Ok);
    CHECK(person.world.characters().traits[person.id.index()] ==
          std::array<std::uint16_t, kTraitSlots>{3, 0, 0, 0, 0, 0, 0, 0});
    for (const std::uint16_t id : std::array<std::uint16_t, 7>{8, 2, 7, 4, 6, 1, 5}) {
        REQUIRE(grant_trait(person.world, catalog, person.id, id) == TraitStatus::Ok);
    }
    const auto full_hash = person.world.compute_hash();
    CHECK(grant_trait(person.world, catalog, person.id, 9) == TraitStatus::SlotsFull);
    CHECK(person.world.compute_hash() == full_hash);
    CHECK(person.world.check_invariants().empty());
}

TEST_CASE("traits: dead and unknown people cannot receive or lose traits") {
    Person person;
    const auto catalog = plain_catalog(1);
    REQUIRE(grant_trait(person.world, catalog, person.id, 1) == TraitStatus::Ok);
    REQUIRE(person.world.die(person.id, CauseOfDeath::OldAge));
    const auto hash = person.world.compute_hash();
    for (const CharacterId id : {person.id, CharacterId{}, CharacterId(100)}) {
        CHECK(grant_trait(person.world, catalog, id, 1) == TraitStatus::NotAlive);
        CHECK(revoke_trait(person.world, catalog, id, 1) == TraitStatus::NotAlive);
        CHECK(skill_breakdown(person.world, catalog, id, Skill::Martial).terms.empty());
        CHECK(attribute_breakdown(person.world, catalog, id, CharacterAttribute::Health).terms.empty());
    }
    CHECK(person.world.compute_hash() == hash);
}

TEST_CASE("traits: opposite traits are refused in either grant order") {
    auto catalog = plain_catalog(2);
    catalog.definitions[0].opposite = 2;
    catalog.definitions[1].opposite = 1;
    REQUIRE(validate(catalog).has_value());
    for (const std::uint16_t first : std::array<std::uint16_t, 2>{1, 2}) {
        Person person;
        const auto second = static_cast<std::uint16_t>(3 - first);
        REQUIRE(grant_trait(person.world, catalog, person.id, first) == TraitStatus::Ok);
        const auto hash = person.world.compute_hash();
        CHECK(grant_trait(person.world, catalog, person.id, second) == TraitStatus::Opposite);
        CHECK(person.world.compute_hash() == hash);
        REQUIRE(revoke_trait(person.world, catalog, person.id, first) == TraitStatus::Ok);
        CHECK(grant_trait(person.world, catalog, person.id, second) == TraitStatus::Ok);
    }
}

TEST_CASE("traits: congenital category and inherited traits share three slots") {
    Person person;
    auto catalog = plain_catalog(5);
    for (std::size_t i = 0; i < 3; ++i) {
        catalog.definitions[i].category = TraitCategory::Congenital;
    }
    catalog.definitions[3].category = TraitCategory::Health;
    catalog.definitions[3].inherit_chance = Fixed32::from_ratio(1, 2);
    REQUIRE(validate(catalog).has_value());
    for (const std::uint16_t id : std::array<std::uint16_t, 3>{1, 2, 3}) {
        REQUIRE(grant_trait(person.world, catalog, person.id, id) == TraitStatus::Ok);
    }
    const auto hash = person.world.compute_hash();
    CHECK(grant_trait(person.world, catalog, person.id, 4) == TraitStatus::CongenitalLimit);
    CHECK(person.world.compute_hash() == hash);
    REQUIRE(grant_trait(person.world, catalog, person.id, 5) == TraitStatus::Ok);
    REQUIRE(revoke_trait(person.world, catalog, person.id, 2) == TraitStatus::Ok);
    CHECK(grant_trait(person.world, catalog, person.id, 4) == TraitStatus::Ok);
}

TEST_CASE("traits: set validation ignores empty slots and rejects inconsistent sets") {
    auto catalog = plain_catalog(9);
    catalog.definitions[0].opposite = 2;
    catalog.definitions[1].opposite = 1;
    CHECK(check_trait_set(catalog, std::array<std::uint16_t, 4>{3, 0, 1, 0}) == TraitStatus::Ok);
    CHECK(check_trait_set(catalog, std::array<std::uint16_t, 2>{1, 1}) == TraitStatus::InvalidSet);
    CHECK(check_trait_set(catalog, std::array<std::uint16_t, 2>{1, 2}) == TraitStatus::Opposite);
    CHECK(check_trait_set(catalog, std::array<std::uint16_t, 1>{10}) == TraitStatus::Unknown);
    CHECK(check_trait_set(catalog, std::array<std::uint16_t, 9>{1, 3, 4, 5, 6, 7, 8, 9, 2}) ==
          TraitStatus::SlotsFull);
}

TEST_CASE("traits: breakdown exposes additive, multiplier and clamp deltas without changing bases") {
    Person person;
    auto catalog = plain_catalog(2);
    auto& first = catalog.definitions[0];
    first.skill_modifiers[static_cast<std::size_t>(Skill::Martial)] = fx(5);
    first.attribute_modifiers[static_cast<std::size_t>(CharacterAttribute::Health)] = fx(-10);
    first.attribute_modifiers[static_cast<std::size_t>(CharacterAttribute::Fertility)] = fx(10);
    first.attribute_modifiers[static_cast<std::size_t>(CharacterAttribute::Stress)] = fx(-20);
    first.attribute_modifiers[static_cast<std::size_t>(CharacterAttribute::Ambition)] = fx(-30);
    first.health_multiplier_permille = 500;
    first.fertility_multiplier_permille = 1500;
    auto& second = catalog.definitions[1];
    second.attribute_modifiers[static_cast<std::size_t>(CharacterAttribute::Health)] = fx(10);
    second.attribute_modifiers[static_cast<std::size_t>(CharacterAttribute::Fertility)] = fx(10);
    second.health_multiplier_permille = 1500;
    second.fertility_multiplier_permille = 500;
    REQUIRE(validate(catalog).has_value());
    person.world.set_character_skill(person.id, Skill::Martial, fx(19));
    const auto bases = person.world.characters();
    REQUIRE(grant_trait(person.world, catalog, person.id, 2) == TraitStatus::Ok);
    REQUIRE(grant_trait(person.world, catalog, person.id, 1) == TraitStatus::Ok);

    const auto martial = skill_breakdown(person.world, catalog, person.id, Skill::Martial);
    CHECK(martial.total == fx(20));
    CHECK(martial.sum_of_terms() == martial.total);
    CHECK(has_kind(martial, CharacterTermKind::Base));
    CHECK(has_kind(martial, CharacterTermKind::Trait));
    CHECK(has_kind(martial, CharacterTermKind::Clamp));
    const auto bonus =
        std::find_if(martial.terms.begin(), martial.terms.end(), [](const CharacterTerm& term) {
            return term.kind == CharacterTermKind::Trait;
        });
    REQUIRE(bonus != martial.terms.end());
    CHECK(bonus->trait == 1);
    CHECK(bonus->value == fx(5));
    for (const auto& [attribute, expected] :
         std::array<std::pair<CharacterAttribute, int>, 4>{{{CharacterAttribute::Health, 60},
                                                            {CharacterAttribute::Fertility, 45},
                                                            {CharacterAttribute::Stress, 0},
                                                            {CharacterAttribute::Ambition, -100}}}) {
        const auto breakdown = attribute_breakdown(person.world, catalog, person.id, attribute);
        CHECK(breakdown.total == fx(expected));
        CHECK(breakdown.sum_of_terms() == breakdown.total);
        if (attribute == CharacterAttribute::Health || attribute == CharacterAttribute::Fertility) {
            CHECK(has_kind(breakdown, CharacterTermKind::Multiplier));
        } else {
            CHECK(has_kind(breakdown, CharacterTermKind::Clamp));
        }
    }
    CHECK(person.world.characters().health == bases.health);
    CHECK(person.world.characters().fertility == bases.fertility);
    CHECK(person.world.characters().skills == bases.skills);
    REQUIRE(revoke_trait(person.world, catalog, person.id, 1) == TraitStatus::Ok);
    REQUIRE(revoke_trait(person.world, catalog, person.id, 2) == TraitStatus::Ok);
    CHECK(person.world.characters() == bases);
    CHECK(attribute_breakdown(person.world, catalog, person.id, CharacterAttribute::Health).total == fx(80));
    CHECK(skill_breakdown(person.world, catalog, person.id, Skill::Martial).total == fx(19));
}

TEST_CASE("traits: cumulative multipliers are bounded and a zero factor cancels the value") {
    Person person(1);
    auto catalog = plain_catalog(3);
    for (auto& def : catalog.definitions) {
        def.health_multiplier_permille = def.id == 3 ? 500 : 10000;
        def.mortality_multiplier_permille = def.health_multiplier_permille;
        def.fertility_multiplier_permille = def.id == 1 ? 0 : 10000;
    }
    REQUIRE(validate(catalog).has_value());
    for (const auto& def : catalog.definitions) {
        REQUIRE(grant_trait(person.world, catalog, person.id, def.id) == TraitStatus::Ok);
    }
    const auto health = attribute_breakdown(person.world, catalog, person.id, CharacterAttribute::Health);
    const auto fertility =
        attribute_breakdown(person.world, catalog, person.id, CharacterAttribute::Fertility);
    CHECK(health.total == fx(5));
    CHECK(health.sum_of_terms() == health.total);
    CHECK(fertility.total == fx(0));
    CHECK(fertility.sum_of_terms() == fertility.total);
    CHECK(trait_mortality_multiplier(catalog, std::array<std::uint16_t, 6>{3, 2, 1, 2, 0, 999}) == 5000);
}

TEST_CASE("traits: subset sweep preserves explanation sums, bounds and grant-order determinism") {
    auto catalog = plain_catalog(6);
    for (std::size_t i = 0; i < catalog.definitions.size(); ++i) {
        auto& def = catalog.definitions[i];
        const int sign = i % 2 == 0 ? 1 : -1;
        def.skill_modifiers.fill(fx(sign * static_cast<int>(i + 1)));
        def.attribute_modifiers.fill(fx(sign * static_cast<int>(15 * (i + 1))));
        def.health_multiplier_permille = i == 0 ? 333 : (i % 2 == 0 ? 2000 : 500);
        def.fertility_multiplier_permille = i % 2 == 0 ? 500 : 2000;
    }
    REQUIRE(validate(catalog).has_value());
    for (std::uint32_t subset = 0; subset < 64; ++subset) {
        CAPTURE(subset);
        Person ascending;
        Person descending;
        for (std::uint16_t id = 1; id <= 6; ++id) {
            if ((subset & (1U << (id - 1))) != 0) {
                REQUIRE(grant_trait(ascending.world, catalog, ascending.id, id) == TraitStatus::Ok);
            }
        }
        for (std::uint16_t id = 6; id > 0; --id) {
            if ((subset & (1U << (id - 1))) != 0) {
                REQUIRE(grant_trait(descending.world, catalog, descending.id, id) == TraitStatus::Ok);
            }
        }
        CHECK(ascending.world.compute_hash() == descending.world.compute_hash());
        for (std::size_t i = 0; i < kSkillCount; ++i) {
            const auto skill = static_cast<Skill>(i);
            const auto a = skill_breakdown(ascending.world, catalog, ascending.id, skill);
            const auto b = skill_breakdown(descending.world, catalog, descending.id, skill);
            CHECK(a.sum_of_terms() == a.total);
            CHECK(a.total >= fx(0));
            CHECK(a.total <= fx(20));
            CHECK(a.total == b.total);
        }
        for (std::size_t i = 0; i < kCharacterAttributeCount; ++i) {
            const auto attribute = static_cast<CharacterAttribute>(i);
            const auto a = attribute_breakdown(ascending.world, catalog, ascending.id, attribute);
            const auto b = attribute_breakdown(descending.world, catalog, descending.id, attribute);
            CHECK(a.sum_of_terms() == a.total);
            CHECK(a.total >= fx(i < static_cast<std::size_t>(CharacterAttribute::Ambition) ? 0 : -100));
            CHECK(a.total <= fx(100));
            CHECK(a.total == b.total);
        }
    }
}

TEST_CASE("traits: commands require the system and creation checks its initial trait set") {
    Person person;
    auto catalog = plain_catalog(2);
    catalog.definitions[0].opposite = 2;
    catalog.definitions[1].opposite = 1;
    const DynastyId other = person.world.create_dynasty(StringId(1));
    const auto hash = person.world.compute_hash();
    for (const DynastyId actor : {DynastyId(0), other}) {
        const auto grant = validate(person.world, Command{actor, cmd::GrantTrait{person.id, 1}}, &catalog);
        const auto revoke = validate(person.world, Command{actor, cmd::RevokeTrait{person.id, 1}}, &catalog);
        REQUIRE_FALSE(grant.has_value());
        REQUIRE_FALSE(revoke.has_value());
        CHECK(grant.error().reason == Rejection::NotAuthorized);
        CHECK(revoke.error().reason == Rejection::NotAuthorized);
    }
    const auto without_catalog = validate(person.world, Command{DynastyId(), cmd::GrantTrait{person.id, 1}});
    REQUIRE_FALSE(without_catalog.has_value());
    CHECK(without_catalog.error().reason == Rejection::InvalidState);

    CharacterInit init;
    init.dynasty = DynastyId(0);
    init.birth_date = Date::from_ymd(1680, 1, 1);
    init.traits = {1, 2};
    const auto opposites = validate(person.world, Command{DynastyId(), cmd::CreatePerson{init}}, &catalog);
    REQUIRE_FALSE(opposites.has_value());
    CHECK(opposites.error().reason == Rejection::InvalidArgument);
    init.traits = {3};
    const auto unknown = validate(person.world, Command{DynastyId(), cmd::CreatePerson{init}}, &catalog);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().reason == Rejection::UnknownEntity);
    CHECK(person.world.compute_hash() == hash);
}

TEST_CASE("traits: pending commands survive saves and external commands round trip through replay") {
    auto definitions = plain_catalog(2);
    definitions.definitions[0].skill_modifiers[static_cast<std::size_t>(Skill::Martial)] = fx(3);
    const std::shared_ptr<const TraitCatalog> catalog =
        std::make_shared<TraitCatalog>(std::move(definitions));
    Simulation original = trait_simulation(catalog);
    Simulation resumed = trait_simulation(catalog);
    ReplayRecorder recorder(7, 0x7A17ULL, original.world().today(), 1);
    for (int tick = 1; tick <= 6; ++tick) {
        if (tick == 5 || tick == 6) {
            const Command command = tick == 5 ? Command{DynastyId(), cmd::GrantTrait{CharacterId(0), 2}}
                                              : Command{DynastyId(), cmd::RevokeTrait{CharacterId(0), 2}};
            REQUIRE(original.submit(command));
            REQUIRE(resumed.submit(command));
        }
        (void)original.tick();
        (void)resumed.tick();
        recorder.after_tick(original);
        if (tick == 1 || tick == 3) {
            REQUIRE(original.pending_internal().size() == 1);
            CHECK(type_of(original.pending_internal()[0]) ==
                  (tick == 1 ? CommandType::GrantTrait : CommandType::RevokeTrait));
            auto loaded = deserialize_save(serialize_save(original.world(), original.pending_internal()));
            REQUIRE(loaded.has_value());
            CHECK(loaded.value().pending_internal == std::vector<Command>(original.pending_internal().begin(),
                                                                          original.pending_internal().end()));
            resumed.restore_state(std::move(loaded.value().world),
                                  std::move(loaded.value().pending_internal));
        }
        CHECK(resumed.world().compute_hash() == original.world().compute_hash());
        if (tick == 2) {
            CHECK(skill_breakdown(original.world(), *catalog, CharacterId(0), Skill::Martial).total ==
                  fx(13));
        }
    }
    const Replay recorded = recorder.finish(original);
    REQUIRE(recorded.commands.size() == 2);
    CHECK(type_of(recorded.commands[0].command) == CommandType::GrantTrait);
    CHECK(type_of(recorded.commands[1].command) == CommandType::RevokeTrait);
    auto decoded = deserialize_replay(serialize_replay(recorded));
    REQUIRE(decoded.has_value());
    // PRP1 stores the six recorded hash parts, while the in-memory checkpoint also has ext parts.
    CHECK(serialize_replay(decoded.value()) == serialize_replay(recorded));
    Simulation replayed = trait_simulation(catalog);
    const auto result = play_replay(replayed, decoded.value());
    CHECK(result.ok);
    CHECK(result.error.empty());
    CHECK(replayed.world().compute_hash() == original.world().compute_hash());
    CHECK(skill_breakdown(original.world(), *catalog, CharacterId(0), Skill::Martial).total == fx(10));
    CHECK(original.world().check_invariants().empty());
}

TEST_CASE("traits: mortality uses effective health and explicit mortality factors") {
    const Date birthday = Date::from_ymd(1703, 1, 1);
    World world(birthday, 7);
    REQUIRE(world.enable_dynasty_mode());
    auto definitions = plain_catalog(2);
    definitions.definitions[0].attribute_modifiers[static_cast<std::size_t>(CharacterAttribute::Health)] =
        fx(-30);
    definitions.definitions[1].mortality_multiplier_permille = 0;
    const std::shared_ptr<const TraitCatalog> catalog =
        std::make_shared<TraitCatalog>(std::move(definitions));
    CharacterInit init;
    init.birth_date = Date::from_ymd(1673, 1, 1);
    init.health = fx(50);
    init.traits = {1};
    const CharacterId frail = world.create_character(init);
    init.traits = {1, 2};
    const CharacterId immune = world.create_character(init);
    DemographyRules rules = kDefaultDemography;
    for (auto& bucket : rules.mortality.buckets) {
        bucket.value = 500;
    }
    rules.low_health_below = 30;
    rules.low_health_mortality_permille = 2000;
    SimulationConfig config;
    config.traits = catalog;
    Simulation simulation(std::move(world), config);
    simulation.add_system(make_mortality_system(rules, catalog));
    (void)simulation.tick();
    REQUIRE(simulation.pending_internal().size() == 1);
    const auto* plan = std::get_if<cmd::PlanDeath>(&simulation.pending_internal()[0].payload);
    REQUIRE(plan != nullptr);
    CHECK(plan->character == frail);
    CHECK(plan->date >= birthday);
    CHECK(plan->date < birthday + 365);
    CHECK(simulation.world().characters().health[frail.index()] == fx(50));
    CHECK(simulation.world().characters().planned_death[immune.index()] == kNoDate);
    pat::core::RngStreams expected(7);
    expected.stream("demography.mortality").advance(4);
    CHECK(simulation.world().rng().save() == expected.save());
}
