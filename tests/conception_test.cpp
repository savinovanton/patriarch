// Conception, pregnancy and birth (E7-T12): the pregnancy table of the World and its save section, the
// commands Conceive and Birth, and the system dynasty.conception.

#include "pat/core/bytes.hpp"
#include "pat/core/rng.hpp"
#include "pat/core/string_interner.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/command_log.hpp"
#include "pat/sim/conception.hpp"
#include "pat/sim/kinship.hpp"
#include "pat/sim/mortality.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace pat::sim;
using pat::core::ByteReader;
using pat::core::ByteWriter;
using pat::core::Fixed32;
using pat::core::RngStreams;

constexpr std::uint64_t kSeed = 7;
constexpr std::uint64_t kFixtureHash = 8525650564880343936ULL;  // total hash of dynasty_v11_pregnancy.psv

Date start_date() {
    return Date::from_ymd(1703, 1, 1);
}

World dynasty_world(Date start = start_date()) {
    World world(start, kSeed);
    REQUIRE(world.enable_dynasty_mode());
    return world;
}

CharacterId add_person(World& world, DynastyId dynasty, Sex sex, Date birth, int health = 50, int skill = 0) {
    CharacterInit init;
    init.dynasty = dynasty;
    init.sex = sex;
    init.birth_date = birth;
    init.health = Fixed32::from_int(health);
    init.skills.fill(Fixed32::from_int(skill));
    const CharacterId id = world.create_character(init);
    REQUIRE(id.valid());
    return id;
}

struct Couple {
    CharacterId husband;
    CharacterId wife;
};

Couple add_couple(World& world, DynastyId dynasty, int husband_year = 1675, int wife_year = 1678) {
    const CharacterId husband = add_person(world, dynasty, Sex::Male, Date::from_ymd(husband_year, 3, 1));
    const CharacterId wife = add_person(world, dynasty, Sex::Female, Date::from_ymd(wife_year, 3, 1));
    REQUIRE(world.marry(husband, wife));
    return {husband, wife};
}

Pregnancy pregnancy_of(CharacterId mother,
                       CharacterId father,
                       Date conceived,
                       Date due,
                       PregnancyEnd end = PregnancyEnd::AtTerm) {
    Pregnancy p;
    p.mother = mother;
    p.father = father;
    p.conceived = conceived;
    p.due = due;
    p.end = end;
    return p;
}

void require_valid(const World& world) {
    const auto problems = world.check_invariants();
    INFO((problems.empty() ? std::string() : problems.front()));
    REQUIRE(problems.empty());
}

std::uint64_t pregnancy_part(const World& world) {
    return world.compute_hash().ext[static_cast<std::size_t>(ExtPart::Pregnancy)];
}

std::vector<std::uint8_t> state_bytes(const World& world) {
    ByteWriter out;
    world.save_state(out);
    return out.take();
}

pat::core::Result<World, pat::core::Error> load_bytes(const std::vector<std::uint8_t>& bytes) {
    ByteReader in(bytes);
    return World::load_state(in);
}

/// Rules that make births happen: certain conception for the peak age, no losses unless asked.
DemographyRules sure_conception() {
    DemographyRules rules = kDefaultDemography;
    rules.base_conception_permille = 1000;
    rules.miscarriage_permille = 0;
    rules.crowding_min_permille = 1000;  // crowding off
    return rules;
}

BirthContent content_of(const DemographyRules& rules) {
    BirthContent content;
    content.rules = rules;
    return content;
}

Simulation make_sim(World world, BirthContent content, bool with_mortality = false) {
    Simulation sim(std::move(world));
    sim.add_system(make_conception_system(content));
    if (with_mortality) {
        sim.add_system(make_mortality_system(content.rules, content.traits));
    }
    return sim;
}

DemographyRules no_mortality(DemographyRules rules) {
    for (auto& bucket : rules.mortality.buckets) {
        bucket.value = 0;
    }
    return rules;
}

/// The Birth commands waiting in the queue of the simulation.
std::vector<cmd::Birth> pending_births(const Simulation& sim) {
    std::vector<cmd::Birth> births;
    for (const Command& command : sim.pending_internal()) {
        if (const auto* birth = std::get_if<cmd::Birth>(&command.payload)) {
            births.push_back(*birth);
        }
    }
    return births;
}

/// The deaths in childbirth queued by the system (the Die after a Birth).
std::int64_t pending_childbirth_deaths(const Simulation& sim) {
    std::int64_t deaths = 0;
    for (const Command& command : sim.pending_internal()) {
        if (const auto* die = std::get_if<cmd::Die>(&command.payload)) {
            deaths += die->cause == CauseOfDeath::Childbirth ? 1 : 0;
        }
    }
    return deaths;
}

Command system_command(CommandPayload payload) {
    return Command{DynastyId(), std::move(payload)};
}

Rejection verdict(const World& world, const Command& command, const TraitCatalog* traits = nullptr) {
    const auto result = validate(world, command, traits);
    return result ? Rejection::None : result.error().reason;
}

void apply_all(World& world, std::vector<Command> commands) {
    CommandLog log;
    const auto stats = process_commands(world, commands, CommandSource::Internal, log);
    REQUIRE(stats.rejected == 0);
}

// ---- name pools and traits for the tests --------------------------------------------------------

GivenName man(std::string id) {
    GivenName n;
    n.id = id;
    n.sex = Sex::Male;
    n.weight = 10;
    n.name_key = "name." + id;
    n.patronymic_son_key = "pat." + id + ".son";
    n.patronymic_daughter_key = "pat." + id + ".daughter";
    return n;
}

GivenName woman(std::string id) {
    GivenName n;
    n.id = id;
    n.sex = Sex::Female;
    n.weight = 10;
    n.name_key = "name." + id;
    return n;
}

std::shared_ptr<const NamePools> make_pools() {
    auto pools = std::make_shared<NamePools>();
    pools->rules = {.parent_bonus = 40, .grandparent_bonus = 25};
    pools->given_names = {
        man("ivan"), man("petr"), man("fedor"), woman("anna"), woman("maria"), woman("fekla")};
    const std::array<LifeStage, 5> stages{
        LifeStage::Child, LifeStage::Adolescent, LifeStage::Prime, LifeStage::Mature, LifeStage::Elder};
    for (std::size_t i = 0; i < stages.size(); ++i) {
        pools->portraits.push_back({static_cast<std::uint16_t>(11 + i), Sex::Male, stages[i]});
        pools->portraits.push_back({static_cast<std::uint16_t>(21 + i), Sex::Female, stages[i]});
    }
    REQUIRE(validate(*pools).has_value());
    return pools;
}

TraitDef health_trait(std::uint16_t id, const char* key, std::uint16_t opposite, Fixed32 inherit_chance) {
    TraitDef trait;
    trait.id = id;
    trait.key = key;
    trait.name_key = std::string("trait.") + key + ".name";
    trait.desc_key = std::string("trait.") + key + ".desc";
    trait.category = TraitCategory::Health;
    trait.opposite = opposite;
    trait.inherit_chance = inherit_chance;
    return trait;
}

constexpr std::uint16_t kRobust = 1;
constexpr std::uint16_t kSickly = 2;
constexpr std::uint16_t kBeautiful = 3;
constexpr std::uint16_t kQuiet = 4;  // not heritable

std::shared_ptr<const TraitCatalog> make_catalog() {
    auto catalog = std::make_shared<TraitCatalog>();
    catalog->definitions = {health_trait(kRobust, "robust", kSickly, Fixed32::from_ratio(4, 10)),
                            health_trait(kSickly, "sickly", kRobust, Fixed32::from_ratio(1, 10)),
                            health_trait(kBeautiful, "beautiful", 0, Fixed32::from_ratio(15, 100)),
                            health_trait(kQuiet, "quiet", 0, Fixed32::zero())};
    REQUIRE(validate(*catalog).has_value());
    return catalog;
}

void give_traits(World& world, CharacterId id, std::initializer_list<std::uint16_t> traits) {
    const std::vector<std::uint16_t> list(traits);
    REQUIRE(world.set_character_traits(id, list));
}

/// `count` mothers (each with a living father of one shared dynasty) whose labour is due the day
/// after `start`. Returns the mothers.
std::vector<CharacterId> add_pregnant_mothers(World& world,
                                              DynastyId dynasty,
                                              CharacterId father,
                                              int count,
                                              int mother_health = 50,
                                              int mother_skill = 0) {
    std::vector<CharacterId> mothers;
    for (int i = 0; i < count; ++i) {
        const CharacterId mother =
            add_person(world, dynasty, Sex::Female, Date::from_ymd(1680, 1, 1), mother_health, mother_skill);
        REQUIRE(world.begin_pregnancy(pregnancy_of(mother, father, world.today() - 200, world.today() + 1)));
        mothers.push_back(mother);
    }
    return mothers;
}

}  // namespace

// =================================================================================================
// the pregnancy table
// =================================================================================================

TEST_CASE("pregnancy: the table keeps rows by mother and refuses what cannot be a pregnancy") {
    World world = dynasty_world();
    const DynastyId dynasty = world.create_dynasty({});
    const CharacterId father = add_person(world, dynasty, Sex::Male, Date::from_ymd(1675, 1, 1));
    const CharacterId m1 = add_person(world, dynasty, Sex::Female, Date::from_ymd(1680, 1, 1));
    const CharacterId m2 = add_person(world, dynasty, Sex::Female, Date::from_ymd(1681, 1, 1));
    const Date today = world.today();

    // a valid row, inserted out of order
    REQUIRE(world.begin_pregnancy(pregnancy_of(m2, father, today - 30, today + 240)));
    REQUIRE(world.begin_pregnancy(pregnancy_of(m1, father, today - 10, today + 260)));
    REQUIRE(world.pregnancies().size() == 2);
    CHECK(world.pregnancies()[0].mother == m1);
    CHECK(world.pregnancies()[1].mother == m2);
    CHECK(world.is_pregnant(m1));
    CHECK(world.pregnancy_of(m2)->father == father);
    CHECK_FALSE(world.is_pregnant(father));
    CHECK_FALSE(world.pregnancy_of(father).has_value());
    require_valid(world);

    // refused: a second child at once, a man as mother, a woman as father, the dead, a stranger
    const CharacterId m3 = add_person(world, dynasty, Sex::Female, Date::from_ymd(1682, 1, 1));
    CHECK_FALSE(world.begin_pregnancy(pregnancy_of(m1, father, today - 1, today + 200)));
    CHECK_FALSE(world.begin_pregnancy(pregnancy_of(father, father, today - 1, today + 200)));
    CHECK_FALSE(world.begin_pregnancy(pregnancy_of(m3, m2, today - 1, today + 200)));
    CHECK_FALSE(world.begin_pregnancy(pregnancy_of(m3, CharacterId(), today - 1, today + 200)));
    CHECK_FALSE(world.begin_pregnancy(pregnancy_of(CharacterId(99), father, today - 1, today + 200)));
    // refused: dates
    CHECK_FALSE(
        world.begin_pregnancy(pregnancy_of(m3, father, today + 1, today + 200)));  // not yet conceived
    CHECK_FALSE(
        world.begin_pregnancy(pregnancy_of(m3, father, today - 5, today - 5)));  // due before it begins
    CHECK_FALSE(world.begin_pregnancy(pregnancy_of(m3, father, today - 5, today + kMaxPregnancyDays)));
    CHECK_FALSE(world.begin_pregnancy(pregnancy_of(m3, father, kNoDate, today + 20)));
    Pregnancy odd = pregnancy_of(m3, father, today - 5, today + 100);
    odd.end = static_cast<PregnancyEnd>(2);
    CHECK_FALSE(world.begin_pregnancy(odd));
    odd.end = PregnancyEnd::AtTerm;
    odd.legitimacy = static_cast<Legitimacy>(3);
    CHECK_FALSE(world.begin_pregnancy(odd));
    CHECK(world.pregnancies().size() == 2);

    // a pregnancy ends once, and hands its row out
    const auto ended = world.end_pregnancy(m1);
    REQUIRE(ended.has_value());
    CHECK(ended->due == today + 260);
    CHECK_FALSE(world.end_pregnancy(m1).has_value());
    CHECK(world.pregnancies().size() == 1);

    // outside dynasty mode there are no pregnancies
    World classic(start_date(), kSeed);
    CHECK_FALSE(classic.begin_pregnancy(pregnancy_of(CharacterId(0), CharacterId(1), today - 1, today + 5)));
}

TEST_CASE("pregnancy: labours come due oldest first, stale entries are dropped, nothing is offered twice") {
    World world = dynasty_world();
    const DynastyId dynasty = world.create_dynasty({});
    const CharacterId father = add_person(world, dynasty, Sex::Male, Date::from_ymd(1675, 1, 1));
    std::vector<CharacterId> mothers;
    for (int i = 0; i < 5; ++i) {
        mothers.push_back(add_person(world, dynasty, Sex::Female, Date::from_ymd(1680, 1, 1)));
    }
    const Date today = world.today();
    REQUIRE(world.begin_pregnancy(pregnancy_of(mothers[3], father, today - 100, today + 10)));
    REQUIRE(world.begin_pregnancy(pregnancy_of(mothers[1], father, today - 100, today + 10)));
    REQUIRE(world.begin_pregnancy(pregnancy_of(mothers[0], father, today - 100, today + 20)));
    REQUIRE(world.begin_pregnancy(pregnancy_of(mothers[2], father, today - 100, today + 5)));
    (void)world.end_pregnancy(mothers[0]);  // its queue entry is stale now

    CHECK(world.take_due_labours(today + 4).empty());
    const auto first = world.take_due_labours(today + 10);
    REQUIRE(first.size() == 3);
    CHECK(first[0] == DueLabour{today + 5, mothers[2]});
    CHECK(first[1] == DueLabour{today + 10, mothers[1]});  // the same day: by mother id
    CHECK(first[2] == DueLabour{today + 10, mothers[3]});
    CHECK(world.take_due_labours(today + 30).empty());  // taken once, stale one dropped
    CHECK(world.pregnancies().size() == 3);             // the rows stay until Birth ends them
}

TEST_CASE("pregnancy: a mother who dies loses the pregnancy, a father who dies does not") {
    World world = dynasty_world();
    const DynastyId dynasty = world.create_dynasty({});
    const Couple couple = add_couple(world, dynasty);
    const Date today = world.today();
    REQUIRE(world.begin_pregnancy(pregnancy_of(couple.wife, couple.husband, today - 60, today + 210)));

    REQUIRE(world.die(couple.husband, CauseOfDeath::Illness));
    CHECK(world.is_pregnant(couple.wife));
    require_valid(world);  // a dead father is a father all the same

    REQUIRE(world.die(couple.wife, CauseOfDeath::Illness));
    CHECK_FALSE(world.is_pregnant(couple.wife));
    CHECK(world.pregnancies().empty());
    CHECK(world.take_due_labours(today + 400).empty());
    require_valid(world);
}

TEST_CASE("pregnancy: the part of the hash exists only while there is a pregnancy") {
    World world = dynasty_world();
    const DynastyId dynasty = world.create_dynasty({});
    const Couple couple = add_couple(world, dynasty);
    const WorldHash before = world.compute_hash();
    CHECK(pregnancy_part(world) == 0);

    REQUIRE(world.begin_pregnancy(
        pregnancy_of(couple.wife, couple.husband, world.today() - 5, world.today() + 265)));
    const WorldHash during = world.compute_hash();
    CHECK(pregnancy_part(world) != 0);
    CHECK(during.total != before.total);
    CHECK(during.characters == before.characters);  // the five fixed parts do not see it
    CHECK(during.dynasties == before.dynasties);

    (void)world.end_pregnancy(couple.wife);
    CHECK(pregnancy_part(world) == 0);
    CHECK(world.compute_hash() == before);
    CHECK(ext_part_name(static_cast<std::size_t>(ExtPart::Pregnancy)) == "pregnancy");

    // every field of a row is part of the hash
    const auto hash_of = [&](const Pregnancy& p) {
        World copy = world;
        REQUIRE(copy.begin_pregnancy(p));
        return pregnancy_part(copy);
    };
    const Date today = world.today();
    const Pregnancy base = pregnancy_of(couple.wife, couple.husband, today - 5, today + 265);
    Pregnancy other = base;
    other.due = today + 266;
    CHECK(hash_of(other) != hash_of(base));
    other = base;
    other.conceived = today - 6;
    CHECK(hash_of(other) != hash_of(base));
    other = base;
    other.end = PregnancyEnd::Miscarriage;
    CHECK(hash_of(other) != hash_of(base));
    other = base;
    other.legitimacy = Legitimacy::Illegitimate;
    CHECK(hash_of(other) != hash_of(base));
}

// =================================================================================================
// save and load
// =================================================================================================

TEST_CASE("pregnancy: a world with pregnancies survives a save and load with the same hash and labours") {
    World world = dynasty_world();
    const DynastyId dynasty = world.create_dynasty({});
    const Couple a = add_couple(world, dynasty);
    const Couple b = add_couple(world, dynasty, 1670, 1676);
    const Date today = world.today();
    REQUIRE(world.begin_pregnancy(pregnancy_of(a.wife, a.husband, today - 100, today + 170)));
    REQUIRE(world.begin_pregnancy(
        pregnancy_of(b.wife, b.husband, today - 40, today + 70, PregnancyEnd::Miscarriage)));

    const std::vector<std::uint8_t> bytes = state_bytes(world);
    auto loaded = load_bytes(bytes);
    REQUIRE(loaded.has_value());
    World& copy = loaded.value();
    CHECK(copy.compute_hash() == world.compute_hash());
    CHECK(state_bytes(copy) == bytes);
    REQUIRE(copy.pregnancies().size() == 2);
    CHECK(copy.pregnancy_of(b.wife)->end == PregnancyEnd::Miscarriage);
    require_valid(copy);

    // the queue is derived: both worlds offer the same labours
    CHECK(copy.take_due_labours(today + 300) == world.take_due_labours(today + 300));
}

TEST_CASE("pregnancy: a file of schema 10 loads without pregnancies, a pregnancy needs schema 11") {
    static_assert(World::kStateSchema >= 11, "pregnancy section support starts at schema 11");

    World world = dynasty_world();
    const DynastyId dynasty = world.create_dynasty({});
    const Couple couple = add_couple(world, dynasty);

    // Empty later extensions are omitted, so this world also has a schema-10 representation.
    std::vector<std::uint8_t> bytes = state_bytes(world);
    ByteReader word(std::span<const std::uint8_t>(bytes.data(), 4));
    REQUIRE(word.u32() == World::kStateSchema);
    bytes[0] = 10;
    auto old = load_bytes(bytes);
    REQUIRE(old.has_value());
    CHECK(old.value().pregnancies().empty());
    CHECK(old.value().compute_hash() == world.compute_hash());

    // with a pregnancy the file has section 4, which a schema 10 file cannot hold
    REQUIRE(world.begin_pregnancy(
        pregnancy_of(couple.wife, couple.husband, world.today() - 5, world.today() + 265)));
    bytes = state_bytes(world);
    REQUIRE(load_bytes(bytes).has_value());
    bytes[0] = 10;
    CHECK_FALSE(load_bytes(bytes).has_value());
}

TEST_CASE("pregnancy: damaged files are refused and never crash") {
    World world = dynasty_world();
    const DynastyId dynasty = world.create_dynasty({});
    const Couple a = add_couple(world, dynasty);
    const Couple b = add_couple(world, dynasty, 1670, 1676);
    const Date today = world.today();
    REQUIRE(world.begin_pregnancy(pregnancy_of(a.wife, a.husband, today - 100, today + 170)));
    REQUIRE(world.begin_pregnancy(pregnancy_of(b.wife, b.husband, today - 40, today + 70)));
    const std::vector<std::uint8_t> bytes = state_bytes(world);

    // every truncation
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        const std::vector<std::uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
        CHECK_FALSE(load_bytes(cut).has_value());
    }
    // every byte of the section (the file ends with it) changed to every kind of value: the result is
    // either an error or a world that passes its invariants
    const std::size_t tail = 4 + 4 + 2 * 18 + 8;
    REQUIRE(bytes.size() > tail);
    for (std::size_t at = bytes.size() - tail; at < bytes.size(); ++at) {
        for (const std::uint8_t value : std::array<std::uint8_t, 6>{0, 1, 2, 3, 0x7f, 0xff}) {
            std::vector<std::uint8_t> damaged = bytes;
            damaged[at] = value;
            auto result = load_bytes(damaged);
            if (result.has_value()) {
                CHECK(result.value().check_invariants().empty());
            }
        }
    }
}

// =================================================================================================
// the commands
// =================================================================================================

TEST_CASE("Conceive: only the system, only in dynasty mode, only for a woman without a child") {
    World world = dynasty_world();
    const DynastyId dynasty = world.create_dynasty({});
    const Couple couple = add_couple(world, dynasty);
    const Date today = world.today();
    const cmd::Conceive ok{
        couple.wife, couple.husband, today, today + 270, PregnancyEnd::AtTerm, Legitimacy::Legitimate};
    CHECK(verdict(world, system_command(ok)) == Rejection::None);
    CHECK(verdict(world, Command{dynasty, ok}) == Rejection::NotAuthorized);

    auto bad = ok;
    bad.mother = couple.husband;
    CHECK(verdict(world, system_command(bad)) == Rejection::InvalidArgument);
    bad = ok;
    bad.father = couple.wife;
    CHECK(verdict(world, system_command(bad)) == Rejection::InvalidArgument);
    bad = ok;
    bad.mother = CharacterId(77);
    CHECK(verdict(world, system_command(bad)) == Rejection::UnknownEntity);
    bad = ok;
    bad.due = today;
    CHECK(verdict(world, system_command(bad)) == Rejection::InvalidArgument);
    bad = ok;
    bad.conceived = today + 1;
    CHECK(verdict(world, system_command(bad)) == Rejection::InvalidArgument);
    bad = ok;
    bad.due = today + kMaxPregnancyDays + 1;
    CHECK(verdict(world, system_command(bad)) == Rejection::InvalidArgument);
    bad = ok;
    bad.end = static_cast<PregnancyEnd>(5);
    CHECK(verdict(world, system_command(bad)) == Rejection::InvalidArgument);

    apply_all(world, {system_command(ok)});
    REQUIRE(world.is_pregnant(couple.wife));
    CHECK(world.pregnancy_of(couple.wife)->due == today + 270);
    CHECK(verdict(world, system_command(ok)) == Rejection::InvalidState);  // already carrying a child
    require_valid(world);

    World classic(start_date(), kSeed);
    CHECK(verdict(classic, system_command(ok)) == Rejection::InvalidState);
}

TEST_CASE("Birth: a mother who is dead or not pregnant, or a child who is not due, is refused") {
    World world = dynasty_world();
    const DynastyId dynasty = world.create_dynasty({});
    const Couple couple = add_couple(world, dynasty);
    const Date today = world.today();
    REQUIRE(world.begin_pregnancy(pregnancy_of(couple.wife, couple.husband, today - 100, today + 10)));

    cmd::Birth birth;
    birth.mother = couple.wife;
    birth.outcome = BirthOutcome::Live;
    birth.newborn.dynasty = dynasty;
    CHECK(verdict(world, system_command(birth)) == Rejection::InvalidState);  // not due yet
    CHECK(verdict(world, Command{dynasty, birth}) == Rejection::NotAuthorized);

    World later = world;
    for (int i = 0; i < 11; ++i) {
        (void)later.advance_day();
    }
    CHECK(verdict(later, system_command(birth)) == Rejection::None);

    // the outcome must fit the pregnancy and the rules of the outcome
    auto wrong = birth;
    wrong.outcome = BirthOutcome::Miscarriage;
    CHECK(verdict(later, system_command(wrong)) == Rejection::InvalidArgument);
    wrong = birth;
    wrong.outcome = static_cast<BirthOutcome>(9);
    CHECK(verdict(later, system_command(wrong)) == Rejection::InvalidArgument);
    wrong = birth;
    wrong.newborn.health = Fixed32::from_int(101);
    CHECK(verdict(later, system_command(wrong)) == Rejection::InvalidArgument);
    wrong = birth;
    wrong.newborn.traits = {3, 1};  // not sorted
    CHECK(verdict(later, system_command(wrong)) == Rejection::InvalidArgument);

    // a mother who is not pregnant, and a mother who is dead
    cmd::Birth stranger = birth;
    stranger.mother = couple.husband;
    CHECK(verdict(later, system_command(stranger)) == Rejection::InvalidState);  // alive, no pregnancy
    CHECK(verdict(later, system_command(stranger)) != Rejection::None);
    REQUIRE(later.die(couple.wife, CauseOfDeath::Illness));
    CHECK(verdict(later, system_command(birth)) == Rejection::UnknownEntity);  // dead mother
    cmd::Birth ghost = birth;
    ghost.mother = CharacterId(500);
    CHECK(verdict(later, system_command(ghost)) == Rejection::UnknownEntity);
}

TEST_CASE(
    "Birth: a living child takes the parents and the legitimacy of the pregnancy and today as the birthday") {
    World world = dynasty_world();
    const DynastyId dynasty = world.create_dynasty({});
    const BranchId branch = world.create_branch(dynasty, {});
    const Couple couple = add_couple(world, dynasty);
    REQUIRE(world.set_character_branch(couple.husband, branch));
    Pregnancy p = pregnancy_of(couple.wife, couple.husband, world.today() - 100, world.today());
    p.legitimacy = Legitimacy::Illegitimate;
    REQUIRE(world.begin_pregnancy(p));

    cmd::Birth birth;
    birth.mother = couple.wife;
    birth.newborn.dynasty = dynasty;
    birth.newborn.branch = branch;
    birth.newborn.sex = Sex::Female;
    birth.newborn.health = Fixed32::from_int(70);
    birth.newborn.birth_date = Date::from_ymd(1600, 1, 1);  // ignored: the child is born today
    birth.newborn.father = CharacterId(55);                 // ignored: the parents are the pregnancy's
    birth.newborn.legitimacy = Legitimacy::Legitimate;      // ignored: the pregnancy decides
    apply_all(world, {system_command(birth)});

    CHECK_FALSE(world.is_pregnant(couple.wife));
    const auto children = kin::children_of(world, couple.wife);
    REQUIRE(children.size() == 1);
    const std::size_t i = children[0].index();
    const CharacterTable& people = world.characters();
    CHECK(people.birth_date[i] == world.today());
    CHECK(people.father[i] == couple.husband);
    CHECK(people.mother[i] == couple.wife);
    CHECK(people.legitimacy[i] == Legitimacy::Illegitimate);
    CHECK(people.sex[i] == Sex::Female);
    CHECK(people.dynasty[i] == dynasty);
    CHECK(people.branch[i] == branch);
    CHECK(world.is_alive(couple.wife));
    CHECK(kin::child_count(world, couple.husband) == 1);
    // the birth is in the chronicle and the child has its first-year roll waiting
    CHECK(world.query_chronicle(dynasty, world.today(), world.today(), ChronicleKind::Birth).size() == 1);
    CHECK(world.life_queue().next_due().has_value());
    require_valid(world);
}

TEST_CASE("Birth: a mother's death in childbirth is a Die after it, a stillbirth leaves no child") {
    const auto deliver = [](PregnancyEnd end, BirthOutcome outcome, bool mother_dies) {
        World world = dynasty_world();
        const DynastyId dynasty = world.create_dynasty({});
        const Couple couple = add_couple(world, dynasty);
        REQUIRE(world.begin_pregnancy(
            pregnancy_of(couple.wife, couple.husband, world.today() - 100, world.today(), end)));
        cmd::Birth birth;
        birth.mother = couple.wife;
        birth.outcome = outcome;
        birth.newborn.dynasty = dynasty;
        std::vector<Command> commands{system_command(birth)};
        if (mother_dies) {
            commands.push_back(system_command(cmd::Die{couple.wife, CauseOfDeath::Childbirth}));
        }
        apply_all(world, commands);
        return std::pair{std::move(world), couple};
    };

    {  // live birth and the mother dies: the child lives, the mother is archived with the cause
        auto [world, couple] = deliver(PregnancyEnd::AtTerm, BirthOutcome::Live, true);
        CHECK(kin::child_count(world, couple.wife) == 1);
        const auto archived = world.archived_person(couple.wife);
        REQUIRE(archived.has_value());
        CHECK(archived->cause == CauseOfDeath::Childbirth);
        CHECK(archived->children.size() == 1);
        CHECK(world.is_alive(couple.husband));
        CHECK_FALSE(world.characters().spouse[couple.husband.index()].valid());  // a widower
        CHECK(world.pregnancies().empty());
        require_valid(world);
    }
    {  // stillbirth: nobody is born, the mother may still die
        auto [world, couple] = deliver(PregnancyEnd::AtTerm, BirthOutcome::Stillborn, true);
        CHECK(kin::child_count(world, couple.wife) == 0);
        CHECK(world.is_archived(couple.wife));
        CHECK(world.living().size() == 1);
        require_valid(world);
    }
    {
        auto [world, couple] = deliver(PregnancyEnd::AtTerm, BirthOutcome::Stillborn, false);
        CHECK(world.is_alive(couple.wife));
        CHECK(kin::child_count(world, couple.wife) == 0);
        CHECK(world.pregnancies().empty());
    }
    {  // a miscarriage ends the pregnancy and nothing else
        auto [world, couple] = deliver(PregnancyEnd::Miscarriage, BirthOutcome::Miscarriage, false);
        CHECK(world.is_alive(couple.wife));
        CHECK(world.pregnancies().empty());
        CHECK(world.living().size() == 2);
    }
}

TEST_CASE("Birth: a child is born although the father died during the pregnancy") {
    World world = dynasty_world();
    const DynastyId dynasty = world.create_dynasty({});
    const Couple couple = add_couple(world, dynasty);
    REQUIRE(
        world.begin_pregnancy(pregnancy_of(couple.wife, couple.husband, world.today() - 100, world.today())));
    REQUIRE(world.die(couple.husband, CauseOfDeath::War));

    cmd::Birth birth;
    birth.mother = couple.wife;
    birth.newborn.dynasty = dynasty;
    REQUIRE(verdict(world, system_command(birth)) == Rejection::None);
    apply_all(world, {system_command(birth)});
    const auto children = kin::children_of(world, couple.husband);  // the dead keep their children
    REQUIRE(children.size() == 1);
    CHECK(world.characters().father[children[0].index()] == couple.husband);
    require_valid(world);
}

// =================================================================================================
// the system: conception
// =================================================================================================

TEST_CASE("dynasty.conception: the month is not a day and the date of the labour is nine months later") {
    CHECK(date_plus_months(Date::from_ymd(1703, 1, 10), 9) == Date::from_ymd(1703, 10, 10));
    CHECK(date_plus_months(Date::from_ymd(1703, 5, 28), 9) == Date::from_ymd(1704, 2, 28));
    CHECK(date_plus_months(Date::from_ymd(1703, 12, 1), 1) == Date::from_ymd(1704, 1, 1));
    CHECK(date_plus_months(Date::from_ymd(1703, 1, 31), 1) == Date::from_ymd(1703, 2, 28));  // no 31 February
    CHECK(date_plus_months(Date::from_ymd(1703, 3, 31), 11) ==
          Date::from_ymd(1704, 2, 29));  // 1704 is a leap year
    CHECK(date_plus_months(Date::from_ymd(1703, 6, 15), 0) == Date::from_ymd(1703, 6, 15));
    CHECK(date_plus_months(Date::from_ymd(1703, 6, 15), 24) == Date::from_ymd(1705, 6, 15));
}

TEST_CASE("dynasty.conception: three draws for every eligible wife, whatever the chance") {
    World world = dynasty_world();
    const DynastyId dynasty = world.create_dynasty({});
    constexpr int kWives = 40;
    for (int i = 0; i < kWives; ++i) {
        (void)add_couple(world, dynasty);
    }
    // not eligible: unmarried women and a man's slot take no draws
    for (int i = 0; i < 10; ++i) {
        (void)add_person(world, dynasty, Sex::Female, Date::from_ymd(1680, 1, 1));
    }
    DemographyRules rules = kDefaultDemography;
    rules.base_conception_permille = 0;  // nobody conceives, so nobody drops out of the pool
    Simulation sim = make_sim(std::move(world), content_of(rules));
    sim.run_days(31);  // Jan 2 ... Feb 1: every slot of the month exactly once

    RngStreams expected(kSeed);
    expected.stream(kConceptionStream).advance(3U * kWives);
    CHECK(sim.world().rng().save() == expected.save());
    CHECK(sim.world().pregnancies().empty());
}

TEST_CASE("dynasty.conception: who can conceive - age, a living husband, no child on the way, the cap") {
    World world = dynasty_world(Date::from_ymd(1703, 1, 1));
    const DynastyId dynasty = world.create_dynasty({});
    const Couple fertile = add_couple(world, dynasty, 1675, 1678);   // 24
    const Couple girl = add_couple(world, dynasty, 1675, 1689);      // 13
    const Couple old = add_couple(world, dynasty, 1650, 1655);       // 47
    const Couple pregnant = add_couple(world, dynasty, 1675, 1679);  // already expecting
    const Couple widow = add_couple(world, dynasty, 1675, 1680);
    const Couple capped = add_couple(world, dynasty, 1675, 1678);
    const Couple one_short = add_couple(world, dynasty, 1675, 1678);
    const CharacterId single = add_person(world, dynasty, Sex::Female, Date::from_ymd(1680, 1, 1));
    REQUIRE(world.begin_pregnancy(
        pregnancy_of(pregnant.wife, pregnant.husband, world.today() - 60, world.today() + 200)));
    REQUIRE(world.die(widow.husband, CauseOfDeath::War));
    for (int k = 0; k < 14; ++k) {  // fourteen children (they need only be born after their parents)
        for (const Couple& family : {capped, one_short}) {
            if (family.husband == one_short.husband && k == 13) {
                continue;
            }
            CharacterInit child;
            child.dynasty = dynasty;
            child.birth_date = Date::from_ymd(1695, 1, 1) + k * 3;
            child.father = family.husband;
            child.mother = family.wife;
            REQUIRE(world.create_character(child).valid());
        }
    }
    REQUIRE(kin::child_count(world, capped.wife) == 14);
    REQUIRE(kin::child_count(world, one_short.wife) == 13);

    Simulation sim = make_sim(std::move(world), content_of(sure_conception()));
    sim.run_days(32);  // the last pending Conceive is applied at the start of the 32nd tick
    const World& after = sim.world();

    CHECK(after.is_pregnant(fertile.wife));
    CHECK(after.is_pregnant(one_short.wife));
    CHECK_FALSE(after.is_pregnant(girl.wife));
    CHECK_FALSE(after.is_pregnant(old.wife));
    CHECK_FALSE(after.is_pregnant(widow.wife));
    CHECK_FALSE(after.is_pregnant(capped.wife));
    CHECK_FALSE(after.is_pregnant(single));
    const auto row = after.pregnancy_of(fertile.wife);
    REQUIRE(row.has_value());
    CHECK(row->father == fertile.husband);
    CHECK(row->legitimacy == Legitimacy::Legitimate);
    CHECK(row->end == PregnancyEnd::AtTerm);
    CHECK(row->due == date_plus_months(row->conceived, 9));
    CHECK(row->conceived >= start_date() + 1);
    CHECK(row->conceived <= start_date() + 31);
    // the pregnancy that existed before is untouched
    CHECK(after.pregnancy_of(pregnant.wife)->due == start_date() + 200);
    require_valid(after);
}

TEST_CASE("dynasty.conception: a pregnancy that is going to end in a miscarriage is shorter") {
    World world = dynasty_world();
    const DynastyId dynasty = world.create_dynasty({});
    constexpr int kWives = 200;
    for (int i = 0; i < kWives; ++i) {
        (void)add_couple(world, dynasty);
    }
    DemographyRules rules = sure_conception();
    rules.miscarriage_permille = 1000;
    rules.crowding_to_living = 1'000'000;
    Simulation sim = make_sim(std::move(world), content_of(rules));
    sim.run_days(32);
    REQUIRE(sim.world().pregnancies().size() == kWives);
    std::array<int, 10> by_month{};
    for (const Pregnancy& p : sim.world().pregnancies()) {
        CHECK(p.end == PregnancyEnd::Miscarriage);
        const Date full_term = date_plus_months(p.conceived, 9);
        CHECK(p.due < full_term);
        CHECK(p.due > p.conceived);
        ++by_month[static_cast<std::size_t>((p.due - p.conceived) / 31)];  // about 1 ... 7
    }
    int months_used = 0;
    for (const int n : by_month) {
        months_used += n > 0 ? 1 : 0;
    }
    CHECK(months_used >= 5);  // spread over the months 1 ... 8, not one fixed point
}

TEST_CASE("dynasty.conception: the fertility multipliers of the traits of both spouses change the chance") {
    auto catalog = std::make_shared<TraitCatalog>(*make_catalog());
    catalog->definitions[kQuiet - 1].fertility_multiplier_permille = 0;        // barren
    catalog->definitions[kBeautiful - 1].fertility_multiplier_permille = 500;  // half as likely
    REQUIRE(validate(*catalog).has_value());
    CHECK(trait_fertility_multiplier(*catalog, std::array<std::uint16_t, 3>{kBeautiful, kQuiet, 0}) == 0);
    CHECK(trait_fertility_multiplier(*catalog, std::array<std::uint16_t, 2>{kBeautiful, 0}) == 500);
    CHECK(trait_fertility_multiplier(*catalog, std::array<std::uint16_t, 1>{kRobust}) == 1000);

    const auto pregnant_of = [&](std::initializer_list<std::uint16_t> wife_traits,
                                 std::initializer_list<std::uint16_t> husband_traits) {
        World world = dynasty_world();
        const DynastyId dynasty = world.create_dynasty({});
        std::vector<Couple> couples;
        for (int i = 0; i < 300; ++i) {
            couples.push_back(add_couple(world, dynasty));
            give_traits(world, couples.back().wife, wife_traits);
            give_traits(world, couples.back().husband, husband_traits);
        }
        BirthContent content = content_of(sure_conception());
        content.traits = catalog;
        Simulation sim = make_sim(std::move(world), content);
        sim.run_days(32);
        return sim.world().pregnancies().size();
    };
    CHECK(pregnant_of({}, {}) == 300);
    CHECK(pregnant_of({kQuiet}, {}) == 0);  // a barren wife
    CHECK(pregnant_of({}, {kQuiet}) == 0);  // a barren husband counts too
    const std::size_t half = pregnant_of({kBeautiful}, {});
    CHECK(half > 110);
    CHECK(half < 190);
    const std::size_t quarter = pregnant_of({kBeautiful}, {kBeautiful});  // 0.5 x 0.5
    CHECK(quarter > 40);
    CHECK(quarter < 110);
}

TEST_CASE("dynasty.conception: the crowding of the world brakes conception, a soft brake") {
    // The same couples in a world that is empty and in one that is full to the end of the scale.
    const auto pregnancies_in = [](int extra_people) {
        World world = dynasty_world();
        const DynastyId dynasty = world.create_dynasty({});
        for (int i = 0; i < 400; ++i) {
            (void)add_couple(world, dynasty);
        }
        for (int i = 0; i < extra_people; ++i) {
            (void)add_person(world, dynasty, Sex::Male, Date::from_ymd(1690, 1, 1));
        }
        DemographyRules rules = no_mortality(kDefaultDemography);
        rules.base_conception_permille = 1000;
        rules.miscarriage_permille = 0;
        rules.crowding_from_living = 1000;
        rules.crowding_to_living = 2000;
        rules.crowding_min_permille = 100;
        Simulation sim = make_sim(std::move(world), content_of(rules));
        sim.run_days(32);
        return sim.world().pregnancies().size();
    };
    const std::size_t roomy = pregnancies_in(0);    // 800 living: no brake
    const std::size_t half = pregnancies_in(700);   // 1500 living: about half
    const std::size_t full = pregnancies_in(1300);  // 2100 living: the floor, a tenth
    CHECK(roomy == 400);
    CHECK(half > 150);
    CHECK(half < 250);
    CHECK(full > 10);  // a soft brake, not a wall
    CHECK(full < 80);
}

// =================================================================================================
// the system: labour
// =================================================================================================

TEST_CASE("dynasty.conception: a labour draws a fixed number of numbers from each stream") {
    const auto catalog = make_catalog();
    const auto pools = make_pools();
    pat::core::StringInterner interner;
    BirthContent content = content_of(kDefaultDemography);
    content.traits = catalog;
    content.names = pools;
    content.given_name_ids = intern_given_names(*pools, interner);

    constexpr std::size_t kHeritable = 3;  // robust, sickly, beautiful
    for (const PregnancyEnd end : {PregnancyEnd::AtTerm, PregnancyEnd::Miscarriage}) {
        World world = dynasty_world();
        const DynastyId dynasty = world.create_dynasty({});
        const Couple couple = add_couple(world, dynasty);
        REQUIRE(world.begin_pregnancy(
            pregnancy_of(couple.wife, couple.husband, world.today() - 100, world.today() + 1, end)));
        Simulation sim = make_sim(std::move(world), content);
        (void)sim.tick();  // the labour is due: the Birth command is queued
        REQUIRE(pending_births(sim).size() == 1);

        // exactly: 3 (birth) + 5 skills + heritable traits (genetics) + 2 (name, portrait) - whatever the end
        RngStreams expected(kSeed);
        expected.stream(kBirthStream).advance(3);
        expected.stream(kGeneticsStream).advance(kSkillCount + kHeritable);
        expected.stream(kNamesStream).advance(2);
        CHECK(sim.world().rng().save() == expected.save());
    }
}

TEST_CASE("dynasty.conception: the sex ratio, stillbirths and the risk to the mother on a large sample") {
    constexpr int kLabours = 12'000;
    DemographyRules rules = kDefaultDemography;  // 105 boys per 100 girls, stillbirth 20, risk 50 (health 50)
    const auto run = [&](int mother_health) {
        World world = dynasty_world();
        const DynastyId dynasty = world.create_dynasty({});
        const CharacterId father = add_person(world, dynasty, Sex::Male, Date::from_ymd(1675, 1, 1));
        (void)add_pregnant_mothers(world, dynasty, father, kLabours, mother_health);
        Simulation sim = make_sim(std::move(world), content_of(rules));
        (void)sim.tick();
        return std::pair{pending_births(sim), pending_childbirth_deaths(sim)};
    };
    // Six standard deviations of a binomial count, in exact integer arithmetic: n p (1 - p) with p in
    // per mille (the band is 6 sigma = sqrt(36 n p (1 - p))).
    const auto within = [](std::int64_t count, std::int64_t trials, std::int64_t p_permille) {
        const std::int64_t deviation = count * 1000 - trials * p_permille;  // in units of 1/1000
        return deviation * deviation <= 36 * trials * p_permille * (1000 - p_permille);
    };

    const auto [births, dying] = run(50);
    REQUIRE(births.size() == kLabours);
    std::int64_t live = 0;
    std::int64_t stillborn = 0;
    std::int64_t boys = 0;
    for (const cmd::Birth& birth : births) {
        stillborn += birth.outcome == BirthOutcome::Stillborn ? 1 : 0;
        if (birth.outcome == BirthOutcome::Live) {
            ++live;
            boys += birth.newborn.sex == Sex::Male ? 1 : 0;
            CHECK(birth.newborn.health == Fixed32::from_int(70));
        }
    }
    CHECK(within(stillborn, kLabours, 20));
    CHECK(within(dying, kLabours, 50));  // 5 %: health 50 is neither weak nor robust
    CHECK(live + stillborn == kLabours);
    CHECK(within(boys, live, male_birth_permille(rules)));  // 105 : 100 -> 512 per mille
    CHECK(boys * 1000 / live >= 500);
    CHECK(boys * 1000 / live <= 525);

    // a robust mother (-1 %) and a weak one (+1 %): 4 % and 6 %, as GDD 01 3.3 asks (3 ... 6 %)
    const std::int64_t robust_dying = run(90).second;
    const std::int64_t weak_dying = run(20).second;
    CHECK(within(robust_dying, kLabours, 40));
    CHECK(within(weak_dying, kLabours, 60));
    CHECK(robust_dying < dying);
    CHECK(dying < weak_dying);
}

TEST_CASE("dynasty.conception: a child gets 0.3 of the average skill of its living parents, +-3") {
    const auto mean_skill = [](int mother_skill, int father_skill, bool father_dies) {
        World world = dynasty_world();
        const DynastyId dynasty = world.create_dynasty({});
        const CharacterId father =
            add_person(world, dynasty, Sex::Male, Date::from_ymd(1675, 1, 1), 50, father_skill);
        (void)add_pregnant_mothers(world, dynasty, father, 2000, 50, mother_skill);
        if (father_dies) {
            REQUIRE(world.die(father, CauseOfDeath::War));
        }
        DemographyRules rules = kDefaultDemography;
        rules.stillbirth_permille = 0;
        Simulation sim = make_sim(std::move(world), content_of(rules));
        (void)sim.tick();
        double sum = 0;
        std::int64_t n = 0;
        for (const cmd::Birth& birth : pending_births(sim)) {
            for (const Fixed32 skill : birth.newborn.skills) {
                CHECK(skill >= Fixed32::zero());
                CHECK(skill <= kSkillMax);
                sum += skill.to_double();
                ++n;
            }
        }
        REQUIRE(n > 0);
        return sum / static_cast<double>(n);
    };
    // both parents at 10: 3 + uniform(-3, 3) -> mean 3, never above 6
    CHECK(mean_skill(10, 10, false) == doctest::Approx(3.0).epsilon(0.03));
    // 10 and 20: the average 15 -> 4.5
    CHECK(mean_skill(10, 20, false) == doctest::Approx(4.5).epsilon(0.03));
    // the father died during the pregnancy and kept no skills: the mother alone counts -> 3
    CHECK(mean_skill(10, 20, true) == doctest::Approx(3.0).epsilon(0.03));
    // untrained parents give a child who is mostly at zero: max(0, uniform(-3, 3)) -> mean 0.75
    CHECK(mean_skill(0, 0, false) == doctest::Approx(0.75).epsilon(0.05));
}

TEST_CASE("dynasty.conception: heritable traits pass on with the chance of their definition") {
    const auto catalog = make_catalog();
    const auto frequencies = [&](std::initializer_list<std::uint16_t> mother_traits,
                                 std::initializer_list<std::uint16_t> father_traits) {
        World world = dynasty_world();
        const DynastyId dynasty = world.create_dynasty({});
        const CharacterId father = add_person(world, dynasty, Sex::Male, Date::from_ymd(1675, 1, 1));
        give_traits(world, father, father_traits);
        for (const CharacterId mother : add_pregnant_mothers(world, dynasty, father, 6000)) {
            give_traits(world, mother, mother_traits);
        }
        BirthContent content = content_of(kDefaultDemography);
        content.rules.stillbirth_permille = 0;
        content.traits = catalog;
        Simulation sim = make_sim(std::move(world), content);
        (void)sim.tick();
        std::array<std::int64_t, 5> count{};  // by trait id, [0] = children
        for (const cmd::Birth& birth : pending_births(sim)) {
            REQUIRE(birth.outcome == BirthOutcome::Live);
            ++count[0];
            const auto& traits = birth.newborn.traits;
            CHECK(traits_are_canonical(traits));
            CHECK(check_trait_set(*catalog, traits) == TraitStatus::Ok);
            for (const std::uint16_t trait : traits) {
                if (trait != 0) {
                    ++count[trait];
                }
            }
        }
        return count;
    };
    const auto near = [](std::int64_t count, std::int64_t total, double p) {
        const double frequency = static_cast<double>(count) / static_cast<double>(total);
        return frequency > p - 0.03 && frequency < p + 0.03;
    };

    // robust from both parents: 0.4 + 0.4 - 0.16 = 0.64; beautiful from one: 0.15; nobody carries sickly
    {
        const auto count = frequencies({kRobust}, {kRobust, kBeautiful});
        CHECK(near(count[kRobust], count[0], 0.64));
        CHECK(near(count[kBeautiful], count[0], 0.15));
        CHECK(count[kSickly] == 0);
        CHECK(count[kQuiet] == 0);  // not heritable
    }
    // one parent robust, the other sickly: they exclude each other in the child (opposites)
    {
        const auto count = frequencies({kRobust}, {kSickly});
        CHECK(count[kRobust] > 0);
        CHECK(count[kSickly] > 0);
        // Both cannot be set: robust 0.4 first (ids ascend), then sickly 0.1 only if robust did not come.
        CHECK(near(count[kRobust], count[0], 0.4));
        CHECK(near(count[kSickly], count[0], 0.6 * 0.1));
    }
    // nobody carries anything: nobody inherits anything
    {
        const auto count = frequencies({kQuiet}, {});
        CHECK(count[kRobust] + count[kSickly] + count[kBeautiful] + count[kQuiet] == 0);
    }
}

TEST_CASE(
    "dynasty.conception: names - no repeat among living brothers, an interned id, a portrait of the sex") {
    const auto pools = make_pools();
    pat::core::StringInterner interner;
    BirthContent content = content_of(kDefaultDemography);
    content.rules.stillbirth_permille = 0;
    content.names = pools;
    content.given_name_ids = intern_given_names(*pools, interner);
    REQUIRE(content.given_name_ids.size() == 6);
    const pat::core::StringId ivan = content.given_name_ids[0];
    const pat::core::StringId petr = content.given_name_ids[1];

    World world = dynasty_world();
    const DynastyId dynasty = world.create_dynasty({});
    const CharacterId father = add_person(world, dynasty, Sex::Male, Date::from_ymd(1670, 1, 1));
    constexpr int kMothers = 300;
    std::vector<CharacterId> mothers;
    for (int i = 0; i < kMothers; ++i) {
        const CharacterId mother = add_person(world, dynasty, Sex::Female, Date::from_ymd(1680, 1, 1));
        REQUIRE(world.begin_pregnancy(pregnancy_of(mother, father, world.today() - 200, world.today() + 1)));
        // two living sons named Ivan and Petr
        for (const pat::core::StringId name : {ivan, petr}) {
            CharacterInit son;
            son.dynasty = dynasty;
            son.birth_date = Date::from_ymd(1700, 1, 1);
            son.father = father;
            son.mother = mother;
            son.name = name;
            REQUIRE(world.create_character(son).valid());
        }
        mothers.push_back(mother);
    }
    Simulation sim = make_sim(std::move(world), content);
    (void)sim.tick();
    const auto births = pending_births(sim);
    REQUIRE(births.size() == kMothers);

    int boys = 0;
    int girls = 0;
    for (const cmd::Birth& birth : births) {
        REQUIRE(birth.outcome == BirthOutcome::Live);
        const CharacterInit& child = birth.newborn;
        REQUIRE(child.name.valid());
        const std::string_view id = interner.view(child.name);
        if (child.sex == Sex::Male) {
            ++boys;
            CHECK(id == "fedor");  // Ivan and Petr are taken by the brothers; the half-siblings of the
                                   // father's other wives count too, which is why every boy is Fedor
            CHECK((child.portrait >= 11 && child.portrait <= 15));
        } else {
            ++girls;
            CHECK((id == "anna" || id == "maria" || id == "fekla"));
            CHECK((child.portrait >= 21 && child.portrait <= 25));
        }
        CHECK(child.portrait == 11 + (child.sex == Sex::Male ? 0 : 10));  // the Child stage portrait
    }
    CHECK(boys > 100);
    CHECK(girls > 100);
}

TEST_CASE("dynasty.conception: the child belongs to the father's house and branch, else to the mother's") {
    World world = dynasty_world();
    const DynastyId d1 = world.create_dynasty({});
    const DynastyId d2 = world.create_dynasty({});
    const BranchId b1 = world.create_branch(d1, {});
    const CharacterId father = add_person(world, d1, Sex::Male, Date::from_ymd(1675, 1, 1));
    REQUIRE(world.set_character_branch(father, b1));
    const CharacterId mother = add_person(world, d2, Sex::Female, Date::from_ymd(1680, 1, 1));
    const CharacterId orphan_father = add_person(world, DynastyId(), Sex::Male, Date::from_ymd(1675, 1, 1));
    const CharacterId mother2 = add_person(world, d2, Sex::Female, Date::from_ymd(1680, 1, 1));
    REQUIRE(world.begin_pregnancy(pregnancy_of(mother, father, world.today() - 200, world.today() + 1)));
    REQUIRE(
        world.begin_pregnancy(pregnancy_of(mother2, orphan_father, world.today() - 200, world.today() + 1)));
    DemographyRules rules = kDefaultDemography;
    rules.stillbirth_permille = 0;
    Simulation sim = make_sim(std::move(world), content_of(rules));
    (void)sim.tick();
    const auto births = pending_births(sim);
    REQUIRE(births.size() == 2);
    CHECK(births[0].mother == mother);
    CHECK(births[0].newborn.dynasty == d1);  // the father's
    CHECK(births[0].newborn.branch == b1);
    CHECK(births[1].mother == mother2);
    CHECK(births[1].newborn.dynasty == d2);  // the father has none: the mother's
    CHECK_FALSE(births[1].newborn.branch.valid());
    (void)sim.tick();  // the Birth commands are applied and accepted
    CHECK(sim.world().pregnancies().empty());
    CHECK(sim.world().check_invariants().empty());
}

// =================================================================================================
// whole runs: determinism, save in the middle, the plateau
// =================================================================================================

namespace {

/// A stand-in for the marriages of E8: every month it marries unmarried men and women of age.
class TestMatchmaker final : public System {
public:
    [[nodiscard]] std::string_view name() const override { return "test.matchmaker"; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Monthly; }
    void run(World& world, TickContext& context) override {
        std::vector<CharacterId> men;
        std::vector<CharacterId> women;
        const CharacterTable& people = world.characters();
        for (const CharacterId id : world.living()) {
            const std::size_t i = id.index();
            if (people.spouse[i].valid() || people.marriages[i] != 0) {
                continue;
            }
            const int age = age_years(people.birth_date[i], context.date);
            if (people.sex[i] == Sex::Male && age >= 16) {
                men.push_back(id);
            } else if (people.sex[i] == Sex::Female && age >= 15) {
                women.push_back(id);
            }
        }
        for (std::size_t k = 0; k < std::min(men.size(), women.size()); ++k) {
            context.emit(system_command(cmd::Marry{men[k], women[k]}));
        }
    }
};

World founders(int couples) {
    World world = dynasty_world();
    const DynastyId dynasty = world.create_dynasty({});
    for (int i = 0; i < couples; ++i) {
        (void)add_couple(world, dynasty, 1672 + i % 8, 1676 + i % 6);
    }
    return world;
}

Simulation make_full_sim(World world, const BirthContent& content, bool matchmaker) {
    Simulation sim(std::move(world));
    sim.add_system(make_conception_system(content));
    sim.add_system(make_mortality_system(content.rules, content.traits));
    if (matchmaker) {
        sim.add_system(std::make_unique<TestMatchmaker>());
    }
    return sim;
}

}  // namespace

TEST_CASE("dynasty.conception: the same seed gives the same history") {
    const auto catalog = make_catalog();
    const auto pools = make_pools();
    const auto run = [&] {
        pat::core::StringInterner interner;
        BirthContent content = content_of(sure_conception());
        content.rules.base_conception_permille = 40;
        content.traits = catalog;
        content.names = pools;
        content.given_name_ids = intern_given_names(*pools, interner);
        Simulation sim = make_full_sim(founders(12), content, true);
        sim.run_days(365 * 12);
        return sim.world().compute_hash();
    };
    const WorldHash first = run();
    CHECK(first == run());
    CHECK(first.ext[static_cast<std::size_t>(ExtPart::Archive)] !=
          0);  // people were born and died on the way
}

TEST_CASE(
    "dynasty.conception: a save in the middle of a pregnancy, and with a Birth waiting, continues the same") {
    const auto catalog = make_catalog();
    const auto pools = make_pools();
    pat::core::StringInterner interner;
    BirthContent content = content_of(sure_conception());
    content.rules.base_conception_permille = 60;
    content.traits = catalog;
    content.names = pools;
    content.given_name_ids = intern_given_names(*pools, interner);

    constexpr int kTotalDays = 365 * 3;
    Simulation continuous = make_full_sim(founders(10), content, true);
    continuous.run_days(kTotalDays);

    int saves_in_pregnancy = 0;
    int saves_with_birth_waiting = 0;
    for (const int save_day : {40, 200, 330, 500, 700, 900}) {
        Simulation original = make_full_sim(founders(10), content, true);
        original.run_days(save_day);
        const bool pregnant = !original.world().pregnancies().empty();
        saves_in_pregnancy += pregnant ? 1 : 0;

        auto loaded = deserialize_save(serialize_save(original.world(), original.pending_internal()));
        REQUIRE(loaded.has_value());
        CHECK(loaded.value().world.compute_hash() == original.world().compute_hash());
        Simulation resumed = make_full_sim(World(original.world().today(), kSeed), content, true);
        resumed.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
        resumed.run_days(kTotalDays - save_day);
        CHECK(resumed.world().compute_hash() == continuous.world().compute_hash());
    }
    CHECK(saves_in_pregnancy >= 3);

    // The day after a labour came due the Birth waits in the queue: stop right there.
    {
        Simulation original = make_full_sim(founders(10), content, true);
        for (int day = 0; day < kTotalDays && saves_with_birth_waiting == 0; ++day) {
            original.run_days(1);
            if (!pending_births(original).empty()) {
                saves_with_birth_waiting = 1;
                auto loaded = deserialize_save(serialize_save(original.world(), original.pending_internal()));
                REQUIRE(loaded.has_value());
                Simulation resumed = make_full_sim(World(original.world().today(), kSeed), content, true);
                resumed.restore_state(std::move(loaded.value().world),
                                      std::move(loaded.value().pending_internal));
                REQUIRE(pending_births(resumed).size() == pending_births(original).size());
                original.run_days(kTotalDays - day - 1);
                resumed.run_days(kTotalDays - day - 1);
                CHECK(resumed.world().compute_hash() == original.world().compute_hash());
                CHECK(original.world().compute_hash() == continuous.world().compute_hash());
            }
        }
    }
    CHECK(saves_with_birth_waiting == 1);
}

TEST_CASE("dynasty.conception: a mother who dies before the labour has no child, one who dies after the "
          "Birth was queued does") {
    const auto setup = [] {
        World world = dynasty_world();
        const DynastyId dynasty = world.create_dynasty({});
        const Couple couple = add_couple(world, dynasty);
        REQUIRE(world.begin_pregnancy(
            pregnancy_of(couple.wife, couple.husband, world.today() - 200, world.today() + 5)));
        return std::pair{std::move(world), couple};
    };
    {  // she dies while the child is on its way
        auto [world, couple] = setup();
        DemographyRules rules = kDefaultDemography;
        rules.stillbirth_permille = 0;
        Simulation sim = make_sim(std::move(world), content_of(rules));
        sim.run_days(2);
        REQUIRE(sim.submit(system_command(cmd::Die{couple.wife, CauseOfDeath::Illness})));
        sim.run_days(10);
        CHECK(sim.world().is_archived(couple.wife));
        CHECK(kin::child_count(sim.world(), couple.wife) == 0);
        CHECK(sim.world().pregnancies().empty());
        CHECK(pending_births(sim).empty());
        for (const LoggedCommand& entry : sim.command_log().entries()) {
            CHECK(entry.accepted());  // no Birth was ever refused: there was none
        }
        CHECK(sim.world().check_invariants().empty());
    }
    {  // she dies on the day after the labour came due: the Birth was queued first and still happens
        auto [world, couple] = setup();
        DemographyRules rules = kDefaultDemography;
        rules.stillbirth_permille = 0;
        Simulation sim = make_sim(std::move(world), content_of(rules));
        sim.run_days(5);  // the labour is due on the 5th day
        REQUIRE(pending_births(sim).size() == 1);
        REQUIRE(sim.submit(system_command(cmd::Die{couple.wife, CauseOfDeath::Illness})));
        sim.run_days(1);
        CHECK(sim.world().is_archived(couple.wife));
        const auto children = kin::children_of(sim.world(), couple.wife);
        REQUIRE(children.size() == 1);
        CHECK(sim.world().is_alive(children[0]));
        CHECK(sim.world().check_invariants().empty());
    }
}

TEST_CASE("dynasty.conception: no exponential growth - the crowding levels the population off") {
    // Rules that would multiply the population fast: 30 % a month, a short crowding scale.
    const auto population_after = [](int crowding_min_permille, int years, int stop_above) {
        DemographyRules rules = kDefaultDemography;
        rules.base_conception_permille = 300;
        rules.crowding_from_living = 150;
        rules.crowding_to_living = 300;
        rules.crowding_min_permille = crowding_min_permille;
        BirthContent content = content_of(rules);
        Simulation sim = make_full_sim(founders(30), content, true);
        std::size_t peak = 0;
        std::vector<std::size_t> by_decade;
        for (int day = 0; day < 365 * years; ++day) {
            sim.run_days(1);
            peak = std::max(peak, sim.world().living().size());
            if (day % 3650 == 3649) {
                by_decade.push_back(sim.world().living().size());
            }
            if (static_cast<int>(peak) > stop_above) {
                break;
            }
        }
        return std::pair{peak, by_decade};
    };

    // without the brake the population grows through the end of the scale (300) and keeps going
    const auto unbounded = population_after(1000, 50, 450);
    CHECK(unbounded.first > 450);

    // with the brake it never gets far past the end of the scale (300) and stops growing
    const auto bounded = population_after(0, 60, 100000);
    CHECK(bounded.first <= 360);
    CHECK(bounded.first >= 120);  // it did grow
    REQUIRE(bounded.second.size() >= 4);
    const std::size_t last = bounded.second.back();
    const std::size_t earlier = bounded.second[bounded.second.size() - 3];
    CHECK(last <= earlier * 13 / 10 + 20);  // the last 20 years changed the size little
    CHECK(last * 13 / 10 + 20 >= earlier);
}

TEST_CASE("pregnancy: the fixture of schema 11 loads, re-saves to the same world and goes on") {
    // tests/data/saves/dynasty_v11_pregnancy.psv was made by the code of E7-T12: six couples (founders(6)),
    // conception and mortality systems, default rules except base_conception_permille = 90 and
    // miscarriage_permille = 300, stopped on 1703-03-03 with three pregnancies on the way.
    const auto file =
        read_file(std::filesystem::path(PATRIARCH_TEST_DATA_DIR) / "saves" / "dynasty_v11_pregnancy.psv");
    REQUIRE(file.has_value());
    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());
    World& world = loaded.value().world;
    CHECK(world.today() == Date::from_ymd(1703, 3, 3));
    REQUIRE(world.pregnancies().size() == 3);
    CHECK(std::count_if(world.pregnancies().begin(), world.pregnancies().end(), [](const Pregnancy& p) {
              return p.end == PregnancyEnd::Miscarriage;
          }) >= 1);
    require_valid(world);
    CHECK(world.compute_hash().total == kFixtureHash);

    auto again = deserialize_save(serialize_save(world, loaded.value().pending_internal));
    REQUIRE(again.has_value());
    CHECK(again.value().world.compute_hash() == world.compute_hash());
    CHECK(state_bytes(again.value().world) == state_bytes(world));

    // it goes on: the labours come due and the births happen
    BirthContent content = content_of(kDefaultDemography);
    content.rules.base_conception_permille = 90;
    content.rules.miscarriage_permille = 300;
    const std::size_t people_before = world.characters().ids.capacity();
    Simulation sim = make_full_sim(World(world.today(), kSeed), content, true);
    sim.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
    sim.run_days(400);
    CHECK(sim.world().characters().ids.capacity() > people_before);  // somebody was born
    for (const Pregnancy& p : sim.world().pregnancies()) {
        CHECK(p.conceived > Date::from_ymd(1703, 3, 3));  // the ones of the file are all over
    }
    CHECK(sim.world().check_invariants().empty());
}
