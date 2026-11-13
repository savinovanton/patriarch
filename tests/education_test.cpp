#include "pat/sim/education.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace pat::sim;
using pat::core::Fixed32;

constexpr std::uint64_t kSeed = 31;

Fixed32 points(int value) {
    return Fixed32::from_int(value);
}

World dynasty_world(Date start) {
    World world(start, kSeed);
    REQUIRE(world.enable_dynasty_mode());
    return world;
}

CharacterId person(World& world, Date birth, DynastyId dynasty = {}, Sex sex = Sex::Male) {
    CharacterInit init;
    init.dynasty = dynasty;
    init.birth_date = birth;
    init.sex = sex;
    init.health = points(70);
    const CharacterId id = world.create_character(init);
    REQUIRE(id.valid());
    return id;
}

/// Six education traits like those of data/traits/m1.pd (ids 1...6).
std::shared_ptr<const TraitCatalog> education_catalog() {
    auto catalog = std::make_shared<TraitCatalog>();
    const auto add = [&](std::uint16_t id, const char* key, Skill a, int va, Skill b, int vb) {
        TraitDef def;
        def.id = id;
        def.key = key;
        def.name_key = std::string("trait.") + key + ".name";
        def.desc_key = std::string("trait.") + key + ".desc";
        def.category = TraitCategory::Education;
        def.skill_modifiers[static_cast<std::size_t>(a)] = points(va);
        if (vb != 0) {
            def.skill_modifiers[static_cast<std::size_t>(b)] = points(vb);
        }
        catalog->definitions.push_back(def);
    };
    add(1, "scholar", Skill::Learning, 3, Skill::Learning, 0);
    add(2, "warrior", Skill::Martial, 3, Skill::Martial, 0);
    add(3, "steward", Skill::Stewardship, 3, Skill::Stewardship, 0);
    add(4, "courtier", Skill::Diplomacy, 1, Skill::Intrigue, 2);
    add(5, "diplomat", Skill::Diplomacy, 3, Skill::Diplomacy, 0);
    add(6, "schemer", Skill::Intrigue, 3, Skill::Intrigue, 0);
    REQUIRE(validate(*catalog).has_value());
    return catalog;
}

constexpr std::uint8_t kLearning = static_cast<std::uint8_t>(Skill::Learning) + 1;

Simulation make_sim(World world,
                    std::vector<DynastyId> players = {},
                    const DemographyRules& rules = kDefaultDemography) {
    const auto catalog = education_catalog();
    SimulationConfig config;
    config.traits = catalog;
    Simulation sim(std::move(world), std::move(config));
    EducationContent content;
    content.rules = rules;
    content.traits = catalog;
    content.player_dynasties = std::move(players);
    sim.add_system(make_education_system(std::move(content)));
    return sim;
}

void run_days(Simulation& sim, int days) {
    for (int i = 0; i < days; ++i) {
        (void)sim.tick();
    }
}

std::size_t count_come_of_age(const Simulation& sim, bool accepted_only = true) {
    std::size_t count = 0;
    for (const LoggedCommand& entry : sim.command_log().entries()) {
        if (std::holds_alternative<cmd::ComeOfAge>(entry.command.payload) &&
            (!accepted_only || entry.accepted())) {
            ++count;
        }
    }
    return count;
}

const cmd::ComeOfAge* pending_come_of_age(const Simulation& sim) {
    for (const Command& command : sim.pending_internal()) {
        if (const auto* c = std::get_if<cmd::ComeOfAge>(&command.payload)) {
            return c;
        }
    }
    return nullptr;
}

Rejection verdict(const World& world, const Command& command, const TraitCatalog* traits = nullptr) {
    const auto result = validate(world, command, traits);
    return result ? Rejection::None : result.error().reason;
}

std::size_t note_count(const World& world, DynastyId dynasty, std::int32_t code) {
    std::size_t count = 0;
    for (const ChronicleEntry& entry : world.chronicle(dynasty)) {
        count += entry.kind == ChronicleKind::Note && entry.param == code ? 1 : 0;
    }
    return count;
}

}  // namespace

// ---- pure functions ------------------------------------------------------------------------------

TEST_CASE("education: monthly gain and outcome follow the rules") {
    const DemographyRules rules = kDefaultDemography;
    CHECK(education_monthly_gain(rules, Fixed32::zero()) == 1);
    CHECK(education_monthly_gain(rules, points(4)) == 1);
    CHECK(education_monthly_gain(rules, points(5)) == 2);
    CHECK(education_monthly_gain(rules, points(10)) == 3);
    CHECK(education_monthly_gain(rules, points(20)) == 5);

    const EducationOutcome none = education_outcome(rules, 0, points(20));
    CHECK(none.bonus == Fixed32::zero());
    CHECK_FALSE(none.earns_trait);

    const EducationOutcome full = education_outcome(rules, 100, Fixed32::zero());
    CHECK(full.bonus == points(2));
    CHECK(full.tutor_bonus == Fixed32::zero());
    CHECK(full.earns_trait);

    // A tutor adds 15 % of his skill, and both parts scale with the level.
    const EducationOutcome tutored = education_outcome(rules, 80, points(10));
    CHECK(tutored.base_bonus == points(2).mul_div(80, 100));
    CHECK(tutored.tutor_bonus == points(10).mul_permille(150).mul_div(80, 100));
    CHECK(tutored.bonus == tutored.base_bonus + tutored.tutor_bonus);
    CHECK(tutored.bonus > Fixed32::from_ratio(279, 100));
    CHECK(tutored.bonus < Fixed32::from_ratio(281, 100));

    // The trait is earned from the configured level, not before.
    CHECK_FALSE(education_outcome(rules, rules.education_trait_min_level - 1, Fixed32::zero()).earns_trait);
    CHECK(education_outcome(rules, rules.education_trait_min_level, Fixed32::zero()).earns_trait);

    // The skill scale caps the sum.
    DemographyRules rich = rules;
    rich.education_bonus_max = 20;
    rich.education_tutor_bonus_permille = 10000;
    CHECK(education_outcome(rich, 100, points(20)).bonus == points(20));
}

TEST_CASE("education: the trait of a focus is the strongest education trait for that skill") {
    const auto catalog = education_catalog();
    CHECK(education_trait_for(*catalog, Skill::Learning) == 1);
    CHECK(education_trait_for(*catalog, Skill::Martial) == 2);
    CHECK(education_trait_for(*catalog, Skill::Stewardship) == 3);
    CHECK(education_trait_for(*catalog, Skill::Diplomacy) == 5);  // diplomat (3) beats courtier (1)
    CHECK(education_trait_for(*catalog, Skill::Intrigue) == 6);   // schemer (3) beats courtier (2)
    CHECK(education_trait_for(TraitCatalog{}, Skill::Learning) == 0);
}

// ---- SetEducation --------------------------------------------------------------------------------

TEST_CASE("SetEducation: every violation is refused with its reason") {
    World world = dynasty_world(Date::from_ymd(1712, 6, 1));
    const DynastyId house = world.create_dynasty({});
    const DynastyId other = world.create_dynasty({});
    const CharacterId child = person(world, Date::from_ymd(1705, 3, 1), house);
    const CharacterId adult = person(world, Date::from_ymd(1670, 1, 1), house);
    const CharacterId minor = person(world, Date::from_ymd(1700, 1, 1), house);
    const CharacterId turning_16 = person(world, Date::from_ymd(1696, 6, 1), house);  // 16 today
    const CharacterId dead = person(world, Date::from_ymd(1660, 1, 1), house);
    REQUIRE(world.die(dead, CauseOfDeath::Illness));

    const auto set = [&](DynastyId actor, CharacterId who, std::uint8_t focus, CharacterId tutor = {}) {
        return Command{actor, cmd::SetEducation{who, focus, tutor}};
    };
    CHECK(verdict(world, set(house, child, 2, adult)) == Rejection::None);
    CHECK(verdict(world, set(DynastyId(), child, 5)) == Rejection::None);  // the system may do it
    CHECK(verdict(world, set(house, child, 1)) == Rejection::None);        // no tutor is fine

    CHECK(verdict(world, set(house, dead, 2)) == Rejection::UnknownEntity);
    CHECK(verdict(world, set(house, CharacterId(9999), 2)) == Rejection::UnknownEntity);
    CHECK(verdict(world, set(other, child, 2)) == Rejection::NotAuthorized);
    CHECK(verdict(world, set(house, adult, 2)) == Rejection::InvalidState);       // already of age
    CHECK(verdict(world, set(house, turning_16, 2)) == Rejection::InvalidState);  // 16 today
    CHECK(verdict(world, set(house, child, 0)) == Rejection::InvalidArgument);
    CHECK(verdict(world, set(house, child, 6)) == Rejection::InvalidArgument);
    CHECK(verdict(world, set(house, child, 2, dead)) == Rejection::UnknownEntity);
    CHECK(verdict(world, set(house, child, 2, CharacterId(9999))) == Rejection::UnknownEntity);
    CHECK(verdict(world, set(house, child, 2, child)) == Rejection::InvalidArgument);
    CHECK(verdict(world, set(house, child, 2, minor)) == Rejection::InvalidState);  // a tutor is of age

    // Without dynasty mode there is no education.
    World plain(Date::from_ymd(1712, 6, 1), kSeed);
    CharacterInit init;
    init.birth_date = Date::from_ymd(1705, 3, 1);
    const CharacterId lonely = plain.create_character(init);
    CHECK(verdict(plain, set(DynastyId(), lonely, 2)) == Rejection::InvalidState);

    // The reasons are understandable.
    const auto result = validate(world, set(house, child, 2, minor));
    REQUIRE_FALSE(result.has_value());
    CHECK(std::string(result.error().detail).find("tutor") != std::string::npos);
}

TEST_CASE("SetEducation: a new focus restarts the progress, the same focus keeps it") {
    World world = dynasty_world(Date::from_ymd(1712, 6, 1));
    const DynastyId house = world.create_dynasty({});
    const CharacterId child = person(world, Date::from_ymd(1705, 3, 1), house);
    const CharacterId tutor_a = person(world, Date::from_ymd(1670, 1, 1), house);
    const CharacterId tutor_b = person(world, Date::from_ymd(1675, 1, 1), house);
    REQUIRE(world.set_character_education(child, Education{kLearning, tutor_a, 40}));

    CommandLog log;
    const std::array<Command, 1> same_focus{Command{house, cmd::SetEducation{child, kLearning, tutor_b}}};
    REQUIRE(process_commands(world, same_focus, CommandSource::External, log).rejected == 0);
    CHECK(world.characters().education_focus[child.index()] == kLearning);
    CHECK(world.characters().education_tutor[child.index()] == tutor_b);
    CHECK(world.characters().education_level[child.index()] == 40);

    const std::array<Command, 1> other_focus{Command{house, cmd::SetEducation{child, 2, tutor_b}}};
    REQUIRE(process_commands(world, other_focus, CommandSource::External, log).rejected == 0);
    CHECK(world.characters().education_focus[child.index()] == 2);
    CHECK(world.characters().education_level[child.index()] == 0);
}

// ---- progress ------------------------------------------------------------------------------------

TEST_CASE("education progress: a point a month, faster with a tutor, capped, only from adolescence") {
    World world = dynasty_world(Date::from_ymd(1712, 1, 1));
    const DynastyId house = world.create_dynasty({});
    const CharacterId alone = person(world, Date::from_ymd(1704, 6, 1), house);   // 7
    const CharacterId taught = person(world, Date::from_ymd(1704, 6, 1), house);  // 7
    const CharacterId infant = person(world, Date::from_ymd(1709, 6, 1), house);  // 2
    const CharacterId tutor = person(world, Date::from_ymd(1670, 1, 1), house);
    world.set_character_skill(tutor, Skill::Learning, points(10));
    REQUIRE(world.set_character_education(alone, Education{kLearning, {}, 0}));
    REQUIRE(world.set_character_education(taught, Education{kLearning, tutor, 0}));
    REQUIRE(world.set_character_education(infant, Education{kLearning, {}, 0}));

    Simulation sim = make_sim(std::move(world), {house});  // a player house: no automatic choice
    run_days(sim, 120);                                    // four monthly slots at least
    const CharacterTable& table = sim.world().characters();
    CHECK(table.education_level[alone.index()] >= 3);
    CHECK(table.education_level[alone.index()] <= 5);
    CHECK(table.education_level[taught.index()] >= 9);  // skill 10 / step 5 = +2: three points a month
    CHECK(table.education_level[taught.index()] <= 15);
    CHECK(table.education_level[taught.index()] > table.education_level[alone.index()]);
    CHECK(table.education_level[infant.index()] == 0);  // under 7 nothing yet

    run_days(sim, 365 * 4);
    CHECK(sim.world().characters().education_level[taught.index()] == kMaxEducationLevel);
    CHECK(sim.world().check_invariants().empty());
}

TEST_CASE("education progress: a tutor who died is dropped, the lessons go on") {
    World world = dynasty_world(Date::from_ymd(1712, 1, 1));
    const DynastyId house = world.create_dynasty({});
    const CharacterId child = person(world, Date::from_ymd(1703, 6, 1), house);
    const CharacterId tutor = person(world, Date::from_ymd(1670, 1, 1), house);
    REQUIRE(world.set_character_education(child, Education{kLearning, tutor, 0}));
    REQUIRE(world.die(tutor, CauseOfDeath::Illness));
    Simulation sim = make_sim(std::move(world), {house});
    run_days(sim, 70);
    CHECK_FALSE(sim.world().characters().education_tutor[child.index()].valid());
    CHECK(sim.world().characters().education_level[child.index()] >= 2);
}

// ---- the automatic choice ------------------------------------------------------------------------

TEST_CASE("education: an AI house chooses by itself, a player house does not") {
    World world = dynasty_world(Date::from_ymd(1712, 1, 1));
    const DynastyId ai = world.create_dynasty({});
    const DynastyId player = world.create_dynasty({});
    const CharacterId ai_child = person(world, Date::from_ymd(1704, 3, 1), ai);
    const CharacterId player_child = person(world, Date::from_ymd(1704, 3, 1), player);
    const CharacterId weak = person(world, Date::from_ymd(1670, 1, 1), ai);
    const CharacterId strong = person(world, Date::from_ymd(1675, 1, 1), ai);
    const CharacterId equal = person(world, Date::from_ymd(1676, 1, 1), ai);
    world.set_character_skill(ai_child, Skill::Martial, points(3));
    world.set_character_skill(ai_child, Skill::Learning, points(1));
    world.set_character_skill(weak, Skill::Martial, points(4));
    world.set_character_skill(strong, Skill::Martial, points(9));
    world.set_character_skill(equal, Skill::Martial, points(9));
    Simulation sim = make_sim(std::move(world), {player});
    run_days(sim, 40);
    const CharacterTable& table = sim.world().characters();
    CHECK(table.education_focus[ai_child.index()] == static_cast<std::uint8_t>(Skill::Martial) + 1);
    CHECK(table.education_tutor[ai_child.index()] == strong);  // the best, the smaller id on a tie
    CHECK(table.education_focus[player_child.index()] == 0);
    CHECK_FALSE(table.education_tutor[player_child.index()].valid());
    (void)weak;
    (void)equal;
}

// ---- coming of age -------------------------------------------------------------------------------

TEST_CASE("coming of age: once, on the 16th birthday, with the bonus and the trait") {
    const Date birthday = Date::from_ymd(1712, 6, 15);
    World world = dynasty_world(Date::from_ymd(1712, 6, 1));
    const DynastyId house = world.create_dynasty({});
    const CharacterId child = person(world, Date::from_ymd(1696, 6, 15), house);
    const CharacterId tutor = person(world, Date::from_ymd(1670, 1, 1), house);
    world.set_character_skill(tutor, Skill::Learning, points(10));
    world.set_character_skill(child, Skill::Learning, points(4));
    REQUIRE(world.set_character_education(child, Education{kLearning, tutor, 80}));
    Simulation sim = make_sim(std::move(world), {house});

    const EducationOutcome expected = education_outcome(kDefaultDemography, 80, points(10));
    for (int day = 0; day < 40; ++day) {
        (void)sim.tick();
        const Date today = sim.world().today();
        if (today < birthday) {
            CHECK(pending_come_of_age(sim) == nullptr);
        }
        if (today == birthday) {
            // Decided on the birthday itself, applied at the start of the next tick.
            const cmd::ComeOfAge* pending = pending_come_of_age(sim);
            REQUIRE(pending != nullptr);
            CHECK(pending->character == child);
            CHECK(pending->trait == 1);  // scholar
            CHECK(pending->bonus == expected.bonus);
        }
    }
    CHECK(count_come_of_age(sim) == 1);
    CHECK(count_come_of_age(sim, false) == 1);
    const CharacterTable& table = sim.world().characters();
    CHECK(table.skills[child.index()][static_cast<std::size_t>(Skill::Learning)] ==
          points(4) + expected.bonus);
    CHECK(std::find(table.traits[child.index()].begin(), table.traits[child.index()].end(), 1) !=
          table.traits[child.index()].end());
    CHECK_FALSE(table.education_tutor[child.index()].valid());  // the lessons are over
    CHECK(table.education_focus[child.index()] == kLearning);
    CHECK(note_count(sim.world(), house, kNoteComeOfAge) == 1);
    // The effective skill adds the trait of the education: 4 + bonus + 3.
    CHECK(skill_breakdown(sim.world(), *education_catalog(), child, Skill::Learning).total ==
          points(4) + expected.bonus + points(3));

    // Nothing happens again, whatever the time.
    run_days(sim, 3 * 365);
    CHECK(count_come_of_age(sim, false) == 1);
    CHECK(note_count(sim.world(), house, kNoteComeOfAge) == 1);
}

TEST_CASE("coming of age: a low level gives a smaller bonus and no trait, no focus gives nothing") {
    World world = dynasty_world(Date::from_ymd(1712, 6, 1));
    const DynastyId house = world.create_dynasty({});
    const CharacterId lazy = person(world, Date::from_ymd(1696, 6, 10), house);
    const CharacterId neglected = person(world, Date::from_ymd(1696, 6, 12), house);
    REQUIRE(world.set_character_education(lazy, Education{kLearning, {}, 40}));
    Simulation sim = make_sim(std::move(world), {house});
    run_days(sim, 30);

    const CharacterTable& table = sim.world().characters();
    CHECK(count_come_of_age(sim) == 2);
    CHECK(table.skills[lazy.index()][static_cast<std::size_t>(Skill::Learning)] ==
          education_outcome(kDefaultDemography, 40, Fixed32::zero()).bonus);
    CHECK(table.traits[lazy.index()] == std::array<std::uint16_t, kTraitSlots>{});
    for (std::size_t s = 0; s < kSkillCount; ++s) {
        CHECK(table.skills[neglected.index()][s] == Fixed32::zero());
    }
    CHECK(table.traits[neglected.index()] == std::array<std::uint16_t, kTraitSlots>{});
}

TEST_CASE("coming of age: nobody who is dead, a tutor who is dead brings no bonus, adults are left alone") {
    World world = dynasty_world(Date::from_ymd(1712, 6, 1));
    const DynastyId house = world.create_dynasty({});
    const CharacterId doomed = person(world, Date::from_ymd(1696, 6, 10), house);
    const CharacterId child = person(world, Date::from_ymd(1696, 6, 12), house);
    const CharacterId tutor = person(world, Date::from_ymd(1670, 1, 1), house);
    const CharacterId grown = person(world, Date::from_ymd(1690, 1, 1), house);
    world.set_character_skill(tutor, Skill::Learning, points(20));
    REQUIRE(world.set_character_education(child, Education{kLearning, tutor, 100}));
    REQUIRE(world.die(doomed, CauseOfDeath::Illness));
    REQUIRE(world.die(tutor, CauseOfDeath::Illness));
    Simulation sim = make_sim(std::move(world), {house});
    run_days(sim, 40);

    CHECK(count_come_of_age(sim, false) == 1);  // only the living child
    CHECK(sim.world().characters().skills[child.index()][static_cast<std::size_t>(Skill::Learning)] ==
          points(2));  // the base bonus only: 100 % of 2 points
    CHECK(sim.world().characters().skills[grown.index()][static_cast<std::size_t>(Skill::Learning)] ==
          Fixed32::zero());
}

TEST_CASE("coming of age: a child born in the simulation comes of age exactly once, 16 years later") {
    World world = dynasty_world(Date::from_ymd(1703, 1, 1));
    const DynastyId house = world.create_dynasty({});
    const CharacterId baby = person(world, Date::from_ymd(1703, 1, 1), house);
    Simulation sim = make_sim(std::move(world), {house});
    run_days(sim, 16 * 365 + 5 + 100);  // the leap days of 16 years are 4 (1704, 08, 12, 16)

    CHECK(count_come_of_age(sim, false) == 1);
    for (const LoggedCommand& entry : sim.command_log().entries()) {
        if (const auto* c = std::get_if<cmd::ComeOfAge>(&entry.command.payload)) {
            // Applied on the day after the 16th birthday (the birthday itself is when it is decided).
            CHECK(c->character == baby);
            CHECK(entry.date == adulthood_date(Date::from_ymd(1703, 1, 1)) + 1);
        }
    }
}

// ---- save, replay, determinism -------------------------------------------------------------------

namespace {

Simulation education_scenario() {
    World world = dynasty_world(Date::from_ymd(1712, 5, 1));
    const DynastyId house = world.create_dynasty({});
    const DynastyId rival = world.create_dynasty({});
    const CharacterId child = person(world, Date::from_ymd(1696, 6, 15), house);
    const CharacterId tutor = person(world, Date::from_ymd(1670, 1, 1), house);
    world.set_character_skill(tutor, Skill::Learning, points(12));
    (void)person(world, Date::from_ymd(1704, 4, 1), rival);  // the automatic choice
    (void)person(world, Date::from_ymd(1697, 2, 1), rival);  // comes of age in February 1713
    REQUIRE(world.set_character_education(child, Education{kLearning, tutor, 60}));
    return make_sim(std::move(world), {house});
}

}  // namespace

TEST_CASE("education: a save before the birthday resumes to the same history") {
    Simulation continuous = education_scenario();
    run_days(continuous, 400);

    Simulation first = education_scenario();
    run_days(first, 20);  // 20 May: before the birthday
    auto loaded = deserialize_save(serialize_save(first.world(), first.pending_internal()));
    REQUIRE(loaded.has_value());
    Simulation resumed = make_sim(World(first.world().today(), kSeed), {DynastyId(0)});
    resumed.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
    run_days(resumed, 380);

    CHECK(resumed.world().compute_hash() == continuous.world().compute_hash());
    CHECK(count_come_of_age(resumed, false) == 2);  // both come of age after the save
    CHECK(count_come_of_age(continuous, false) == 2);
}

TEST_CASE("education: a world loaded just after the birthday does not repeat it") {
    Simulation first = education_scenario();
    run_days(first, 50);  // 20 June: the 15th is behind
    REQUIRE(count_come_of_age(first, false) == 1);
    auto loaded = deserialize_save(serialize_save(first.world(), first.pending_internal()));
    REQUIRE(loaded.has_value());
    Simulation resumed = make_sim(World(first.world().today(), kSeed), {DynastyId(0)});
    resumed.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
    run_days(resumed, 60);
    // The second one (February 1713) is not due yet; the first one is not offered again.
    CHECK(count_come_of_age(resumed, false) == 0);
}

TEST_CASE("education: SetEducation and the coming of age replay to the same hashes") {
    constexpr std::uint64_t kScenario = 4242;
    Simulation recorded = education_scenario();
    ReplayRecorder recorder(kSeed, kScenario, recorded.world().today(), 10);
    const DynastyId house = recorded.world().characters().dynasty[0];
    for (int day = 0; day < 60; ++day) {
        if (day == 3) {
            REQUIRE(recorded.submit(Command{house, cmd::SetEducation{CharacterId(0), 2, CharacterId(1)}}));
        }
        (void)recorded.tick();
        recorder.after_tick(recorded);
    }
    const Replay replay = recorder.finish(recorded);
    REQUIRE(replay.commands.size() == 1);
    CHECK(type_of(replay.commands[0].command) == CommandType::SetEducation);
    // The change of the focus (Learning -> Martial) restarted the education: no scholar trait.
    CHECK(count_come_of_age(recorded) == 1);

    Simulation fresh = education_scenario();
    const ReplayResult result = play_replay(fresh, replay);
    CHECK(result.error.empty());
    CHECK(result.ok);
    CHECK(fresh.world().compute_hash() == recorded.world().compute_hash());

    auto decoded = deserialize_replay(serialize_replay(replay));
    REQUIRE(decoded.has_value());
    Simulation from_file = education_scenario();
    CHECK(play_replay(from_file, decoded.value()).ok);
}
