// Kinship (E7-T03, ADR-0006 D2/D6): parents and child lists, spouses and former spouses, the
// godparent reservation, the invariants of GDD 01 section 13.3, the "kin" section of the save
// (schema 3) and the schema-2 fixture that must keep loading.

#include "pat/core/bytes.hpp"
#include "pat/core/compress.hpp"
#include "pat/core/rng.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/command_log.hpp"
#include "pat/sim/kinship.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/system.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace pat::sim;
using pat::core::ByteReader;
using pat::core::ByteWriter;
using pat::core::Pcg32;
using pat::core::StringId;

namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFU;

std::filesystem::path fixture_path(const char* name) {
    return std::filesystem::path(PATRIARCH_TEST_DATA_DIR) / "saves" / name;
}

void require_valid(const World& world) {
    const auto problems = world.check_invariants();
    for (const auto& p : problems) {
        MESSAGE(p);
    }
    REQUIRE(problems.empty());
}

bool mentions(const std::vector<std::string>& problems, const char* text) {
    return std::any_of(problems.begin(), problems.end(), [&](const std::string& p) {
        return p.find(text) != std::string::npos;
    });
}

std::vector<std::uint8_t> state_bytes(const World& world) {
    ByteWriter out;
    world.save_state(out);
    return out.take();
}

pat::core::Result<World, pat::core::Error> load(std::span<const std::uint8_t> bytes) {
    ByteReader in(bytes);
    auto result = World::load_state(in);
    if (result.has_value() && !in.at_end()) {
        return pat::core::Result<World, pat::core::Error>::err(pat::core::Error{99, "trailing bytes"});
    }
    return result;
}

constexpr std::size_t ext_index(ExtPart part) {
    return static_cast<std::size_t>(part);
}

/// Tests corrupt a World on purpose, which the API does not allow: the columns are const.
CharacterTable& table_of(World& world) {
    return const_cast<CharacterTable&>(world.characters());
}

World dynasty_world(std::uint64_t seed = 3, std::uint32_t dynasties = 3) {
    World world(Date::from_ymd(1703, 1, 1), seed);
    REQUIRE(world.enable_dynasty_mode());
    for (std::uint32_t d = 0; d < dynasties; ++d) {
        (void)world.create_dynasty(StringId(d));
    }
    return world;
}

CharacterId add(World& world,
                Sex sex,
                Date birth,
                CharacterId father = CharacterId(),
                CharacterId mother = CharacterId(),
                DynastyId dynasty = DynastyId(0)) {
    CharacterInit init;
    init.dynasty = dynasty;
    init.birth_date = birth;
    init.sex = sex;
    init.father = father;
    init.mother = mother;
    const CharacterId id = world.create_character(init);
    REQUIRE(id.valid());
    return id;
}

Date year(int y, int month = 1, int day = 1) {
    return Date::from_ymd(y, month, day);
}

}  // namespace

// =================================================================================================
// the schema-2 fixture
// =================================================================================================

TEST_CASE("migration: the schema-2 dynasty-mode fixture loads and keeps its hash") {
    // Made by the code of E7-T02 (state schema 2), before any kinship change:
    //   stressgen --profile year1 --seed 7 --characters 240 --dynasties 8 --provinces 24
    //             --armies 0 --schemes 0 --contracts 0 --events 0 --output dynasty_v2.psv
    const auto file = read_file(fixture_path("dynasty_v2.psv"));
    REQUIRE(file.has_value());
    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());
    const World& world = loaded.value().world;
    require_valid(world);

    const WorldHash hash = world.compute_hash();
    CHECK(hash.total == 0x3aa123d9ea55bb03ULL);
    CHECK(hash.calendar == 0x8c7f2e1322674e31ULL);
    CHECK(hash.rng == 0xc6df27438252f6c7ULL);
    CHECK(hash.dynasties == 0x60d711459d77ae89ULL);
    CHECK(hash.provinces == 0x26b46309da939932ULL);
    CHECK(hash.characters == 0x76bb6b7db86975feULL);
    CHECK(world.dynasty_mode());
    CHECK(world.living().size() == 240);

    // No marriages in the old file, so the kinship part is empty and the family is read from the
    // parent columns: the lists of children are derived on load.
    CHECK(hash.ext[ext_index(ExtPart::Kinship)] == 0);
    std::size_t children = 0;
    for (const CharacterId id : world.living()) {
        children += kin::child_count(world, id);
    }
    CHECK(children > 100);  // stressgen gives every later member of a house the founding couple
}

TEST_CASE("migration: schema 3 writes the schema-2 fixture back unchanged apart from the schema word") {
    const auto file = read_file(fixture_path("dynasty_v2.psv"));
    REQUIRE(file.has_value());
    // Container header: magic(4) version(2) flags(2) size(8) hash(8) compressed(8) | zstd frame.
    ByteReader header(file.value());
    (void)header.bytes(4);
    (void)header.u16();
    (void)header.u16();
    const std::uint64_t size = header.u64();
    (void)header.u64();
    const std::uint64_t compressed = header.u64();
    const auto frame = header.bytes(static_cast<std::size_t>(compressed));
    auto old_payload = pat::core::decompress(frame, static_cast<std::size_t>(size));
    REQUIRE(old_payload.has_value());
    const std::vector<std::uint8_t>& old_bytes = old_payload.value();
    ByteReader old_schema(old_bytes);
    CHECK(old_schema.u32() == 2);

    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());
    const std::vector<std::uint8_t> now = state_bytes(loaded.value().world);
    REQUIRE(old_bytes.size() >= now.size());
    CHECK(std::equal(now.begin() + 4, now.end(), old_bytes.begin() + 4));  // no kin section appears
    ByteReader schema(std::span<const std::uint8_t>(now.data(), 4));
    CHECK(schema.u32() == World::kStateSchema);
}

// =================================================================================================
// parents and the lists of children
// =================================================================================================

TEST_CASE("kinship: children are listed eldest first, ties by id, whatever the order of creation") {
    World world = dynasty_world();
    const CharacterId father = add(world, Sex::Male, year(1650));
    const CharacterId mother = add(world, Sex::Female, year(1655));
    const CharacterId other_mother = add(world, Sex::Female, year(1660));
    // ids ascend, birth dates do not
    const CharacterId c1 = add(world, Sex::Male, year(1680), father, mother);
    const CharacterId c2 = add(world, Sex::Female, year(1675), father, mother);
    const CharacterId c3 = add(world, Sex::Male, year(1675), father, mother);  // same day as c2: id decides
    const CharacterId c4 = add(world, Sex::Male, year(1690), father);          // only a father
    const CharacterId c5 = add(world, Sex::Female, year(1685), father, other_mother);
    const CharacterId c6 = add(world, Sex::Male, year(1688), CharacterId(), other_mother);
    require_valid(world);

    using Ids = std::vector<CharacterId>;
    CHECK(kin::children_of(world, father) == Ids{c2, c3, c1, c5, c4});
    CHECK(kin::children_of(world, mother) == Ids{c2, c3, c1});
    CHECK(kin::children_of(world, other_mother) == Ids{c5, c6});
    CHECK(kin::children_of(world, c1).empty());
    CHECK(kin::child_count(world, father) == 5);
    CHECK(kin::child_count(world, c4) == 0);

    CHECK(kin::siblings_of(world, c1) == Ids{c2, c3, c5, c4});      // full and half siblings, eldest first
    CHECK(kin::siblings_of(world, c5) == Ids{c2, c3, c1, c6, c4});  // through the father and the mother
    CHECK(kin::siblings_of(world, c6) == Ids{c5});
    CHECK(kin::siblings_of(world, father).empty());

    // The lists are a derived index: rebuilding them from the columns changes nothing.
    World rebuilt = world;
    rebuilt.rebuild_derived_indexes();
    CHECK(rebuilt.characters() == world.characters());
}

TEST_CASE("kinship: queries answer nothing for people that do not exist and in E1 mode") {
    World world = dynasty_world();
    const CharacterId a = add(world, Sex::Male, year(1650));
    const CharacterId b = add(world, Sex::Male, year(1680), a);
    CHECK(kin::children_of(world, CharacterId()).empty());
    CHECK(kin::children_of(world, CharacterId(999)).empty());
    CHECK(kin::siblings_of(world, CharacterId(999)).empty());
    CHECK(kin::ancestors_of(world, CharacterId(999)).empty());
    CHECK(kin::descendants_of(world, CharacterId()).empty());
    CHECK(kin::child_count(world, CharacterId(999)) == 0);
    CHECK_FALSE(kin::is_ancestor_of(world, a, CharacterId(999)));
    CHECK(kin::is_ancestor_of(world, a, b));
    CHECK_FALSE(kin::is_ancestor_of(world, b, a));
    CHECK_FALSE(kin::is_ancestor_of(world, a, a));

    World e1(year(1703), 1);
    (void)e1.create_dynasty(StringId(0));
    CharacterInit init;
    init.dynasty = DynastyId(0);
    init.birth_date = year(1650);
    const CharacterId p = e1.create_character(init);
    init.birth_date = year(1680);
    init.father = p;
    const CharacterId child = e1.create_character(init);
    CHECK(e1.characters().father[child.index()] == p);  // E1 keeps the reference as given
    CHECK(kin::children_of(e1, p).empty());             // but keeps no family tree
    CHECK(kin::ancestors_of(e1, child).empty());
    require_valid(e1);
}

TEST_CASE("kinship: dynasty mode keeps only parents that can be parents") {
    World world = dynasty_world();
    const CharacterId man = add(world, Sex::Male, year(1650));
    const CharacterId woman = add(world, Sex::Female, year(1655));
    const CharacterId young = add(world, Sex::Male, year(1700));

    // wrong sex
    const CharacterId a = add(world, Sex::Male, year(1690), woman, man);
    CHECK_FALSE(world.characters().father[a.index()].valid());
    CHECK_FALSE(world.characters().mother[a.index()].valid());
    // born after the child, and on the very same day
    const CharacterId b = add(world, Sex::Female, year(1690), young, woman);
    CHECK_FALSE(world.characters().father[b.index()].valid());
    CHECK(world.characters().mother[b.index()] == woman);
    const CharacterId c = add(world, Sex::Male, year(1650), man, woman);
    CHECK_FALSE(world.characters().father[c.index()].valid());
    CHECK_FALSE(world.characters().mother[c.index()].valid());  // born in 1655, not before 1650
    // a reference that points nowhere
    CharacterInit init;
    init.dynasty = DynastyId(0);
    init.birth_date = year(1690);
    init.father = CharacterId(5000);
    init.mother = CharacterId();
    const CharacterId d = world.create_character(init);
    CHECK_FALSE(world.characters().father[d.index()].valid());
    require_valid(world);
    CHECK(kin::children_of(world, man).empty());
    CHECK(kin::children_of(world, woman) == std::vector<CharacterId>{b});
}

TEST_CASE("kinship: only an E1 world with a sound family may enter dynasty mode") {
    auto build = [](bool broken) {
        World e1(year(1703), 1);
        (void)e1.create_dynasty(StringId(0));
        CharacterInit init;
        init.dynasty = DynastyId(0);
        init.birth_date = year(1650);
        const CharacterId father = e1.create_character(init);
        init.father = father;
        init.birth_date = broken ? year(1640) : year(1680);  // the "child" is older than the father
        (void)e1.create_character(init);
        return e1;
    };
    World good = build(false);
    CHECK(good.enable_dynasty_mode());
    require_valid(good);
    CHECK(kin::children_of(good, CharacterId(0)) == std::vector<CharacterId>{CharacterId(1)});

    World bad = build(true);
    CHECK_FALSE(bad.enable_dynasty_mode());
    CHECK_FALSE(bad.dynasty_mode());
    require_valid(bad);  // still a valid E1 world
}

// =================================================================================================
// ancestors and descendants
// =================================================================================================

TEST_CASE("kinship: ancestors and descendants come in generation order, each person once") {
    World world = dynasty_world();
    using Ids = std::vector<CharacterId>;
    // Two houses; the brothers and sisters of one house marry into the other (double cousins), so
    // the ancestors of the heir collapse: every grandparent is reached twice.
    const CharacterId gf1 = add(world, Sex::Male, year(1620));
    const CharacterId gm1 = add(world, Sex::Female, year(1625));
    const CharacterId gf2 = add(world, Sex::Male, year(1622));
    const CharacterId gm2 = add(world, Sex::Female, year(1627));
    const CharacterId f = add(world, Sex::Male, year(1650), gf1, gm1);
    const CharacterId aunt = add(world, Sex::Female, year(1648), gf1, gm1);  // elder than her brother f
    const CharacterId m = add(world, Sex::Female, year(1652), gf2, gm2);
    const CharacterId uncle = add(world, Sex::Male, year(1649), gf2, gm2);
    const CharacterId cousin_f = add(world, Sex::Male, year(1675), f, m);
    const CharacterId cousin_m = add(world, Sex::Female, year(1676), uncle, aunt);
    const CharacterId heir = add(world, Sex::Male, year(1700), cousin_f, cousin_m);
    require_valid(world);

    CHECK(kin::ancestors_of(world, heir) == Ids{cousin_f, cousin_m, f, m, uncle, aunt, gf1, gm1, gf2, gm2});
    CHECK(kin::ancestors_of(world, heir, 1) == Ids{cousin_f, cousin_m});
    CHECK(kin::ancestors_of(world, heir, 2) == Ids{cousin_f, cousin_m, f, m, uncle, aunt});
    CHECK(kin::ancestors_of(world, heir, 0).empty());
    CHECK(kin::ancestors_of(world, gf1).empty());

    CHECK(kin::descendants_of(world, gf1) == Ids{aunt, f, cousin_m, cousin_f, heir});
    CHECK(kin::descendants_of(world, gf1, 1) == Ids{aunt, f});
    CHECK(kin::descendants_of(world, aunt) == Ids{cousin_m, heir});
    CHECK(kin::descendants_of(world, heir).empty());
    CHECK(kin::is_ancestor_of(world, gf2, heir));
    CHECK(kin::is_ancestor_of(world, uncle, heir));
    CHECK_FALSE(kin::is_ancestor_of(world, cousin_f, cousin_m));
}

TEST_CASE("kinship: the branch of a newborn follows the parent who stays in the dynasty") {
    World world = dynasty_world();
    const BranchId b0 = world.create_branch(DynastyId(0), StringId(70));
    const BranchId b1 = world.create_branch(DynastyId(1), StringId(71));
    const CharacterId father = add(world, Sex::Male, year(1650), CharacterId(), CharacterId(), DynastyId(0));
    const CharacterId mother =
        add(world, Sex::Female, year(1655), CharacterId(), CharacterId(), DynastyId(1));
    REQUIRE(world.set_character_branch(father, b0));
    REQUIRE(world.set_character_branch(mother, b1));
    CHECK(kin::inherited_branch(world, father, mother, DynastyId(0)) == b0);
    CHECK(kin::inherited_branch(world, father, mother, DynastyId(1)) == b1);
    CHECK_FALSE(kin::inherited_branch(world, father, mother, DynastyId(2)).valid());
    CHECK_FALSE(kin::inherited_branch(world, CharacterId(), CharacterId(), DynastyId(0)).valid());
}

// =================================================================================================
// marriage primitives and commands
// =================================================================================================

TEST_CASE("marriage: marry and end_marriage keep both partners in step, three marriages at most") {
    World world = dynasty_world();
    const CharacterId a = add(world, Sex::Male, year(1650));
    const CharacterId b = add(world, Sex::Female, year(1655));
    const CharacterId c = add(world, Sex::Female, year(1660));
    const CharacterId d = add(world, Sex::Female, year(1662));
    const CharacterId e = add(world, Sex::Female, year(1664));
    const auto& t = world.characters();

    CHECK(world.compute_hash().ext[ext_index(ExtPart::Kinship)] == 0);
    CHECK_FALSE(world.marry(a, a));
    CHECK_FALSE(world.marry(a, CharacterId(999)));
    CHECK_FALSE(world.end_marriage(a));  // not married

    REQUIRE(world.marry(a, b));
    CHECK(t.spouse[a.index()] == b);
    CHECK(t.spouse[b.index()] == a);
    CHECK(t.marriages[a.index()] == 1);
    CHECK(t.marriages[b.index()] == 1);
    CHECK_FALSE(world.marry(a, c));  // a is married
    CHECK_FALSE(world.marry(c, b));  // b is married
    CHECK(world.compute_hash().ext[ext_index(ExtPart::Kinship)] != 0);
    require_valid(world);

    REQUIRE(world.end_marriage(b));  // either partner may be named
    CHECK_FALSE(t.spouse[a.index()].valid());
    CHECK_FALSE(t.spouse[b.index()].valid());
    CHECK(t.ex_spouses[a.index()][0] == b);
    CHECK(t.ex_spouses[b.index()][0] == a);
    CHECK(t.marriages[a.index()] == 1);  // the counter never goes down
    CHECK_FALSE(world.marry(a, b));      // the same two do not marry twice
    CHECK_FALSE(world.marry(b, a));
    require_valid(world);

    REQUIRE(world.marry(a, c));
    REQUIRE(world.end_marriage(a));
    REQUIRE(world.marry(d, a));
    REQUIRE(world.end_marriage(a));
    CHECK(t.marriages[a.index()] == 3);
    CHECK(t.ex_spouses[a.index()] == std::array<CharacterId, kMaxExSpouses>{b, c, d});  // order of ending
    CHECK_FALSE(world.marry(a, e));  // a fourth marriage is not possible
    CHECK(world.marry(e, b));        // but the others are free
    require_valid(world);

    // save and load keep everything
    auto loaded = load(state_bytes(world));
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().compute_hash() == world.compute_hash());
    CHECK(loaded.value().characters() == world.characters());
}

TEST_CASE("marriage: dynasty mode only, the dead do not marry") {
    World e1(year(1703), 1);
    (void)e1.create_dynasty(StringId(0));
    CharacterInit init;
    init.dynasty = DynastyId(0);
    init.birth_date = year(1650);
    const CharacterId a = e1.create_character(init);
    init.sex = Sex::Female;
    const CharacterId b = e1.create_character(init);
    CHECK_FALSE(e1.marry(a, b));
    CHECK_FALSE(e1.set_character_godparents(a, {b, CharacterId()}));
    CHECK(validate(e1, Command{DynastyId(), cmd::Marry{a, b}}).error().reason == Rejection::InvalidState);
    CHECK(validate(e1, Command{DynastyId(), cmd::EndMarriage{a, MarriageEnd::Annulment}}).error().reason ==
          Rejection::InvalidState);

    World world = dynasty_world();
    const CharacterId x = add(world, Sex::Male, year(1650));
    const CharacterId y = add(world, Sex::Female, year(1655));
    CHECK(world.enable_dynasty_mode());  // idempotent
    CHECK(world.marry(x, y));
}

TEST_CASE("marriage: commands are validated, applied and survive the command log") {
    World world = dynasty_world();
    const CharacterId a = add(world, Sex::Male, year(1650));
    const CharacterId b = add(world, Sex::Female, year(1655));
    const CharacterId c = add(world, Sex::Female, year(1656));
    const DynastyId player(0);

    auto verdict = [&](const cmd::Marry& m, DynastyId actor = DynastyId()) {
        return validate(world, Command{actor, m});
    };
    CHECK(verdict(cmd::Marry{a, b}).has_value());
    CHECK(verdict(cmd::Marry{a, b}, player).error().reason == Rejection::NotAuthorized);
    CHECK(verdict(cmd::Marry{a, a}).error().reason == Rejection::InvalidArgument);
    CHECK(verdict(cmd::Marry{a, CharacterId(500)}).error().reason == Rejection::UnknownEntity);
    CHECK(verdict(cmd::Marry{CharacterId(), b}).error().reason == Rejection::UnknownEntity);

    CommandLog log;
    const std::vector<Command> script = {
        Command{DynastyId(), cmd::Marry{a, b}},
        Command{DynastyId(), cmd::Marry{a, c}},                           // a is married: rejected
        Command{player, cmd::EndMarriage{a, MarriageEnd::Tonsure}},       // players do not end marriages
        Command{DynastyId(), cmd::EndMarriage{c, MarriageEnd::Tonsure}},  // c is not married
        Command{DynastyId(), cmd::EndMarriage{a, MarriageEnd::Tonsure}},
        Command{DynastyId(), cmd::Marry{a, b}},  // they were married before
        Command{DynastyId(), cmd::Marry{c, a}},
    };
    const ApplyStats stats = process_commands(world, script, CommandSource::Internal, log);
    CHECK(stats.applied == 3);
    CHECK(stats.rejected == 4);
    CHECK(world.characters().spouse[a.index()] == c);
    CHECK(world.characters().ex_spouses[a.index()][0] == b);
    CHECK(world.characters().marriages[a.index()] == 2);
    require_valid(world);

    auto round = CommandLog::deserialize(log.serialize());
    REQUIRE(round.has_value());
    CHECK(round.value() == log);

    // A reason that does not exist never gets into a command, whichever way it arrives.
    ByteWriter w;
    write_command(w, Command{DynastyId(), cmd::EndMarriage{a, MarriageEnd::Tonsure}});
    std::vector<std::uint8_t> bytes = w.take();
    bytes.back() = 2;
    ByteReader in(bytes);
    Command ignored;
    CHECK_FALSE(read_command(in, ignored));
    CHECK(validate(world, Command{DynastyId(), cmd::EndMarriage{a, static_cast<MarriageEnd>(9)}})
              .error()
              .reason == Rejection::InvalidArgument);
}

TEST_CASE("commands: in dynasty mode a parent must be older than the child") {
    World world = dynasty_world();
    const CharacterId father = add(world, Sex::Male, year(1690));
    cmd::CreateCharacter c{DynastyId(0), year(1690), Sex::Male, father, CharacterId()};
    CHECK(validate(world, Command{DynastyId(), c}).error().reason == Rejection::InvalidArgument);
    c.birth_date = year(1691);
    CHECK(validate(world, Command{DynastyId(), c}).has_value());

    cmd::CreatePerson p;
    p.init.dynasty = DynastyId(0);
    p.init.birth_date = year(1680);
    p.init.father = father;
    CHECK(validate(world, Command{DynastyId(), p}).error().reason == Rejection::InvalidArgument);
    p.init.birth_date = year(1700);
    CHECK(validate(world, Command{DynastyId(), p}).has_value());

    // E1 mode keeps accepting what it always accepted (the demo depends on it).
    World e1(year(1703), 1);
    (void)e1.create_dynasty(StringId(0));
    CharacterInit init;
    init.dynasty = DynastyId(0);
    init.birth_date = year(1690);
    const CharacterId old_father = e1.create_character(init);
    cmd::CreateCharacter same_day{DynastyId(0), year(1690), Sex::Male, old_father, CharacterId()};
    CHECK(validate(e1, Command{DynastyId(), same_day}).has_value());
}

TEST_CASE("godparents: reserved, validated, saved and hashed") {
    World world = dynasty_world();
    const CharacterId a = add(world, Sex::Male, year(1650));
    const CharacterId b = add(world, Sex::Female, year(1655));
    const CharacterId c = add(world, Sex::Male, year(1660));
    CHECK(world.characters().godparent[a.index()] == std::array<CharacterId, kGodparentSlots>{});
    CHECK_FALSE(world.set_character_godparents(a, {a, CharacterId()}));     // not oneself
    CHECK_FALSE(world.set_character_godparents(a, {b, b}));                 // two different people
    CHECK_FALSE(world.set_character_godparents(a, {CharacterId(500), b}));  // who exist
    CHECK_FALSE(world.set_character_godparents(CharacterId(500), {b, c}));
    CHECK(world.compute_hash().ext[ext_index(ExtPart::Kinship)] == 0);
    REQUIRE(world.set_character_godparents(a, {b, c}));
    REQUIRE(world.set_character_godparents(c, {CharacterId(), a}));
    CHECK(world.compute_hash().ext[ext_index(ExtPart::Kinship)] != 0);
    require_valid(world);
    auto loaded = load(state_bytes(world));
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().characters() == world.characters());
    CHECK(loaded.value().compute_hash() == world.compute_hash());
}

// =================================================================================================
// invariants
// =================================================================================================

namespace {

/// A small married family with children, in dynasty mode.
World family() {
    World world = dynasty_world();
    const CharacterId f = add(world, Sex::Male, year(1650));
    const CharacterId m = add(world, Sex::Female, year(1655));
    const CharacterId x = add(world, Sex::Male, year(1651));
    const CharacterId y = add(world, Sex::Female, year(1656));
    REQUIRE(world.marry(f, m));
    REQUIRE(world.marry(x, y));
    REQUIRE(world.end_marriage(x));
    (void)add(world, Sex::Male, year(1680), f, m);
    (void)add(world, Sex::Female, year(1682), f, m);
    return world;
}

/// Applies `corrupt` to a copy; the invariants must report it, and a save of the damaged state
/// must not load.
template <class Corrupt>
void expect_rejected(const char* what, Corrupt corrupt, const char* message = nullptr) {
    World world = family();
    require_valid(world);
    corrupt(world);
    const auto problems = world.check_invariants();
    INFO(what);
    CHECK_FALSE(problems.empty());
    if (message != nullptr) {
        CHECK(mentions(problems, message));
    }
    CHECK_FALSE(load(state_bytes(world)).has_value());
}

}  // namespace

TEST_CASE("invariants: a damaged family is reported and refused by the loader") {
    // ids: 0 f, 1 m, 2 x, 3 y, 4 and 5 children of f and m
    expect_rejected(
        "father = child (cycle)", [](World& w) { table_of(w).father[0] = CharacterId(4); }, "cycle");
    expect_rejected(
        "longer cycle",
        [](World& w) {
            table_of(w).father[0] = CharacterId(4);
            table_of(w).father[4] = CharacterId();
            table_of(w).mother[4] = CharacterId(1);
            table_of(w).mother[1] = CharacterId(5);
            table_of(w).mother[5] = CharacterId(1);
        },
        "cycle");
    expect_rejected("own parent", [](World& w) { table_of(w).father[0] = CharacterId(0); }, "own parent");
    expect_rejected(
        "father is female", [](World& w) { table_of(w).father[4] = CharacterId(1); }, "wrong sex");
    expect_rejected("mother is male", [](World& w) { table_of(w).mother[4] = CharacterId(0); }, "wrong sex");
    expect_rejected(
        "child older than the parent",
        [](World& w) { table_of(w).birth_date[4] = year(1600); },
        "not younger");
    expect_rejected(
        "child born on the parent's day",
        [](World& w) { table_of(w).birth_date[4] = year(1650); },
        "not younger");
    expect_rejected("parent outside the table", [](World& w) { table_of(w).father[4] = CharacterId(900); });
    expect_rejected(
        "asymmetric spouse", [](World& w) { table_of(w).spouse[1] = CharacterId(); }, "symmetric");
    expect_rejected(
        "spouse of somebody else", [](World& w) { table_of(w).spouse[0] = CharacterId(2); }, "symmetric");
    expect_rejected("married to oneself", [](World& w) { table_of(w).spouse[0] = CharacterId(0); }, "itself");
    expect_rejected(
        "spouse outside the table", [](World& w) { table_of(w).spouse[0] = CharacterId(900); }, "not alive");
    expect_rejected(
        "more marriages than partners", [](World& w) { table_of(w).marriages[0] = 0; }, "fewer marriages");
    expect_rejected("too many marriages", [](World& w) { table_of(w).marriages[0] = 4; }, "more than");
    expect_rejected(
        "former spouse is not mutual", [](World& w) { table_of(w).ex_spouses[3] = {}; }, "symmetric");
    expect_rejected("former spouse listed twice", [](World& w) {
        table_of(w).ex_spouses[2] = {CharacterId(3), CharacterId(3), CharacterId()};
        table_of(w).marriages[2] = 2;
    });
    expect_rejected(
        "former spouses not packed",
        [](World& w) { table_of(w).ex_spouses[2] = {CharacterId(), CharacterId(3), CharacterId()}; },
        "packed");
    expect_rejected(
        "current spouse is also a former one",
        [](World& w) {
            table_of(w).ex_spouses[0] = {CharacterId(1), CharacterId(), CharacterId()};
            table_of(w).ex_spouses[1] = {CharacterId(0), CharacterId(), CharacterId()};
            table_of(w).marriages[0] = 2;
            table_of(w).marriages[1] = 2;
        },
        "invalid former spouse");
    expect_rejected(
        "godparent is oneself",
        [](World& w) { table_of(w).godparent[4] = {CharacterId(4), CharacterId()}; },
        "godparent");
    expect_rejected(
        "two equal godparents",
        [](World& w) { table_of(w).godparent[4] = {CharacterId(1), CharacterId(1)}; },
        "godparent");
    expect_rejected(
        "godparent that does not exist",
        [](World& w) { table_of(w).godparent[4] = {CharacterId(900), CharacterId()}; },
        "godparent");
}

TEST_CASE("invariants: the derived child lists are checked against the parent columns") {
    auto damaged = [](auto corrupt) {
        World world = family();
        corrupt(table_of(world));
        return world.check_invariants();
    };
    CHECK(mentions(damaged([](CharacterTable& t) { t.first_child[0] = CharacterId(); }), "child lists hold"));
    CHECK(mentions(damaged([](CharacterTable& t) { t.next_by_father[4] = CharacterId(); }),
                   "child lists hold"));
    CHECK(mentions(damaged([](CharacterTable& t) {
                       t.first_child[0] = CharacterId(5);  // youngest first
                       t.next_by_father[5] = CharacterId(4);
                       t.next_by_father[4] = CharacterId();
                   }),
                   "damaged"));
    CHECK(mentions(damaged([](CharacterTable& t) { t.next_by_father[5] = CharacterId(4); }),
                   "damaged"));  // loop
    CHECK(mentions(damaged([](CharacterTable& t) { t.first_child[2] = CharacterId(4); }),
                   "damaged"));  // not his child
    CHECK(mentions(damaged([](CharacterTable& t) { t.first_child[0] = CharacterId(900); }), "damaged"));
    CHECK(mentions(damaged([](CharacterTable& t) { t.next_by_mother.pop_back(); }), "differ in size"));
}

TEST_CASE("invariants: kinship data in E1 mode is refused, the demo never has any") {
    World e1(year(1703), 1);
    (void)e1.create_dynasty(StringId(0));
    CharacterInit init;
    init.dynasty = DynastyId(0);
    init.birth_date = year(1650);
    const CharacterId a = e1.create_character(init);
    const CharacterId b = e1.create_character(init);
    require_valid(e1);
    table_of(e1).spouse[a.index()] = b;
    table_of(e1).spouse[b.index()] = a;
    table_of(e1).marriages[a.index()] = 1;
    table_of(e1).marriages[b.index()] = 1;
    CHECK(mentions(e1.check_invariants(), "outside dynasty mode"));
    CHECK_FALSE(load(state_bytes(e1)).has_value());
    table_of(e1).first_child[0] = CharacterId(1);
    CHECK(mentions(e1.check_invariants(), "not empty outside dynasty mode"));
}

// =================================================================================================
// property tests on random family trees (GDD 01 section 13.3)
// =================================================================================================

namespace {

struct Builder {
    World world;
    Pcg32 rng;

    explicit Builder(std::uint64_t seed) : world(Date::from_ymd(1703, 1, 1), seed), rng(seed, 0x6B1Dull) {
        REQUIRE(world.enable_dynasty_mode());
        for (std::uint32_t d = 0; d < 4; ++d) {
            (void)world.create_dynasty(StringId(d));
        }
    }

    /// A random older person of the wanted sex, or invalid. Roughly one pick in ten ignores the sex
    /// and age rules to exercise the clearing of impossible parents.
    CharacterId pick_parent(Sex sex, Date child_birth, bool careless) {
        const auto count = static_cast<std::uint32_t>(world.characters().ids.capacity());
        if (count == 0) {
            return CharacterId();
        }
        const std::uint32_t start = rng.bounded(count);
        for (std::uint32_t k = 0; k < count; ++k) {
            const CharacterId id((start + k) % count);
            if (careless) {
                return id;
            }
            const auto& t = world.characters();
            if (t.sex[id.index()] == sex && t.birth_date[id.index()] + 15 * 365 <= child_birth) {
                return id;
            }
        }
        return CharacterId();
    }

    void add_people(int n) {
        for (int i = 0; i < n; ++i) {
            CharacterInit init;
            init.dynasty = DynastyId(rng.bounded(4));
            init.sex = rng.chance_permille(500) ? Sex::Male : Sex::Female;
            init.birth_date = Date::from_ymd(1600, 1, 1) + rng.range(0, 37000);
            const bool careless = rng.chance_permille(100);
            if (rng.chance_permille(850)) {
                init.father = pick_parent(Sex::Male, init.birth_date, careless);
            }
            if (rng.chance_permille(850)) {
                init.mother = pick_parent(Sex::Female, init.birth_date, careless);
            }
            REQUIRE(world.create_character(init).valid());
        }
    }

    void change_marriages(int tries) {
        const auto count = static_cast<std::uint32_t>(world.characters().ids.capacity());
        for (int i = 0; i < tries; ++i) {
            const CharacterId a(rng.bounded(count));
            const CharacterId b(rng.bounded(count));
            if (rng.chance_permille(300)) {
                (void)world.end_marriage(a);
            } else {
                (void)world.marry(a, b);
            }
            if (rng.chance_permille(100)) {
                (void)world.set_character_godparents(
                    a, {CharacterId(rng.bounded(count)), CharacterId(rng.bounded(count))});
            }
        }
    }
};

using Ids = std::vector<CharacterId>;

bool born_before(const World& w, CharacterId a, CharacterId b) {
    const auto& t = w.characters();
    return t.birth_date[a.index()] < t.birth_date[b.index()] ||
           (t.birth_date[a.index()] == t.birth_date[b.index()] && a < b);
}

Ids oracle_children(const World& w, CharacterId p) {
    const auto& t = w.characters();
    Ids out;
    for (std::uint32_t i = 0; i < t.ids.capacity(); ++i) {
        if (t.father[i] == p || t.mother[i] == p) {
            out.push_back(CharacterId(i));
        }
    }
    std::sort(out.begin(), out.end(), [&](CharacterId a, CharacterId b) { return born_before(w, a, b); });
    return out;
}

Ids oracle_ancestors(const World& w, CharacterId who) {
    const auto& t = w.characters();
    Ids out;
    Ids level{who};
    while (!level.empty()) {
        Ids next;
        for (const CharacterId person : level) {
            for (const CharacterId parent : {t.father[person.index()], t.mother[person.index()]}) {
                if (parent.valid() && parent != who &&
                    std::find(out.begin(), out.end(), parent) == out.end()) {
                    out.push_back(parent);
                    next.push_back(parent);
                }
            }
        }
        level = next;
    }
    return out;
}

Ids oracle_descendants(const World& w, CharacterId who) {
    Ids out;
    Ids level{who};
    while (!level.empty()) {
        Ids next;
        for (const CharacterId person : level) {
            for (const CharacterId child : oracle_children(w, person)) {
                if (child != who && std::find(out.begin(), out.end(), child) == out.end()) {
                    out.push_back(child);
                    next.push_back(child);
                }
            }
        }
        level = next;
    }
    return out;
}

void check_family_properties(const World& w) {
    const auto& t = w.characters();
    REQUIRE(w.check_invariants().empty());
    const auto capacity = static_cast<std::uint32_t>(t.ids.capacity());
    for (std::uint32_t i = 0; i < capacity; ++i) {
        const CharacterId id(i);
        // a child is younger than its parents, and a parent has the right sex
        if (t.father[i].valid()) {
            REQUIRE(t.birth_date[t.father[i].index()] < t.birth_date[i]);
            REQUIRE(t.sex[t.father[i].index()] == Sex::Male);
        }
        if (t.mother[i].valid()) {
            REQUIRE(t.birth_date[t.mother[i].index()] < t.birth_date[i]);
            REQUIRE(t.sex[t.mother[i].index()] == Sex::Female);
        }
        // spouses are symmetric and counted
        if (t.spouse[i].valid()) {
            REQUIRE(t.spouse[t.spouse[i].index()] == id);
        }
        std::size_t partners = t.spouse[i].valid() ? 1 : 0;
        for (const CharacterId ex : t.ex_spouses[i]) {
            partners += ex.valid() ? 1 : 0;
        }
        REQUIRE(t.marriages[i] >= partners);
        REQUIRE(t.marriages[i] <= kMaxMarriages);

        // the queries agree with a scan of the columns
        REQUIRE(kin::children_of(w, id) == oracle_children(w, id));
        REQUIRE(kin::child_count(w, id) == oracle_children(w, id).size());
        const Ids ancestors = kin::ancestors_of(w, id);
        REQUIRE(ancestors == oracle_ancestors(w, id));
        REQUIRE(kin::descendants_of(w, id) == oracle_descendants(w, id));
        // no cycles: nobody is their own ancestor; every ancestor is older
        REQUIRE(std::find(ancestors.begin(), ancestors.end(), id) == ancestors.end());
        for (const CharacterId a : ancestors) {
            REQUIRE(t.birth_date[a.index()] < t.birth_date[i]);
        }
        // siblings: everybody who shares a parent, exactly once, in the order of the lists
        Ids siblings = kin::siblings_of(w, id);
        Ids expected;
        for (const CharacterId parent : {t.father[i], t.mother[i]}) {
            if (parent.valid()) {
                for (const CharacterId c : oracle_children(w, parent)) {
                    if (c != id && std::find(expected.begin(), expected.end(), c) == expected.end()) {
                        expected.push_back(c);
                    }
                }
            }
        }
        std::sort(expected.begin(), expected.end(), [&](CharacterId a, CharacterId b) {
            return born_before(w, a, b);
        });
        REQUIRE(siblings == expected);
    }
}

}  // namespace

TEST_CASE("property: random family trees keep the invariants of GDD 01 section 13.3") {
    for (std::uint64_t seed = 1; seed <= 16; ++seed) {
        INFO("seed " << seed);
        Builder b(seed);
        b.add_people(260);
        b.change_marriages(300);
        b.add_people(80);
        b.change_marriages(200);
        check_family_properties(b.world);
        CHECK(b.world.compute_hash().ext[ext_index(ExtPart::Kinship)] != 0);
        // the builder produced real trees: some families, some marriages, some depth
        std::size_t married = 0;
        std::size_t deepest = 0;
        for (const CharacterId id : b.world.living()) {
            married += b.world.characters().marriages[id.index()] > 0 ? 1 : 0;
            deepest = std::max(deepest, kin::ancestors_of(b.world, id).size());
        }
        CHECK(married > 20);
        CHECK(deepest > 4);
    }
}

TEST_CASE("property: the lists do not depend on how they were built, saved or loaded") {
    for (std::uint64_t seed = 21; seed <= 28; ++seed) {
        INFO("seed " << seed);
        Builder b(seed);
        b.add_people(220);
        b.change_marriages(250);

        // incremental lists equal lists rebuilt from the columns
        World rebuilt = b.world;
        rebuilt.rebuild_derived_indexes();
        CHECK(rebuilt.characters() == b.world.characters());

        // save and load: same hash, same bytes, same queries
        const std::vector<std::uint8_t> bytes = state_bytes(b.world);
        auto loaded = load(bytes);
        REQUIRE(loaded.has_value());
        const World& copy = loaded.value();
        CHECK(copy.compute_hash() == b.world.compute_hash());
        CHECK(copy.characters() == b.world.characters());
        CHECK(state_bytes(copy) == bytes);
        for (const CharacterId id : b.world.living()) {
            REQUIRE(kin::children_of(copy, id) == kin::children_of(b.world, id));
            REQUIRE(kin::descendants_of(copy, id) == kin::descendants_of(b.world, id));
        }
        check_family_properties(copy);

        // through the container too
        auto container = deserialize_save(serialize_save(b.world));
        REQUIRE(container.has_value());
        CHECK(container.value().world.compute_hash() == b.world.compute_hash());
    }
}

TEST_CASE("property: a saved tree continues exactly like the uninterrupted one") {
    for (std::uint64_t seed = 31; seed <= 36; ++seed) {
        INFO("seed " << seed);
        Builder original(seed);
        original.add_people(150);
        original.change_marriages(150);

        auto loaded = load(state_bytes(original.world));
        REQUIRE(loaded.has_value());
        Builder resumed(seed);  // only to get a Builder shell: replace its world and generator
        resumed.world = std::move(loaded).value();
        resumed.rng = original.rng;
        REQUIRE(resumed.world.compute_hash() == original.world.compute_hash());

        for (int round = 0; round < 4; ++round) {
            original.add_people(30);
            resumed.add_people(30);
            original.change_marriages(60);
            resumed.change_marriages(60);
            (void)original.world.advance_day();
            (void)resumed.world.advance_day();
            REQUIRE(resumed.world.compute_hash() == original.world.compute_hash());
        }
        CHECK(resumed.world.characters() == original.world.characters());
        check_family_properties(resumed.world);
    }
}

TEST_CASE("property: the same seed gives the same tree, another seed another one") {
    Builder a(5);
    Builder b(5);
    Builder c(6);
    for (Builder* builder : {&a, &b, &c}) {
        builder->add_people(120);
        builder->change_marriages(100);
    }
    CHECK(a.world.compute_hash() == b.world.compute_hash());
    CHECK(a.world.compute_hash() != c.world.compute_hash());
}

// =================================================================================================
// the kin section of the save: hand-made, damaged, fuzzed
// =================================================================================================

namespace {

struct KinRow {
    std::uint32_t id = 0;
    std::uint32_t spouse = kNone;
    std::uint8_t marriages = 0;
    std::uint32_t ex[3] = {kNone, kNone, kNone};
    std::uint32_t godparent[2] = {kNone, kNone};
};

/// A dynasty-mode world with six people and no data that would be saved in a section; its payload
/// ends with the extension block header (flags 1, zero sections).
World bare_world() {
    World world = dynasty_world();
    for (int i = 0; i < 6; ++i) {
        (void)add(world, i % 2 == 0 ? Sex::Male : Sex::Female, year(1650 + i));
    }
    return world;
}

/// The payload of `bare_world()` with one kin section appended (or a section of another id).
std::vector<std::uint8_t> with_kin_section(const std::vector<KinRow>& rows,
                                           std::uint16_t id = 3,
                                           std::uint16_t version = 1,
                                           int declared_rows = -1,
                                           const std::vector<std::uint8_t>& extra = {}) {
    const std::vector<std::uint8_t> plain = state_bytes(bare_world());
    REQUIRE(plain.size() > 6);
    ByteWriter body;
    body.u32(static_cast<std::uint32_t>(declared_rows < 0 ? static_cast<int>(rows.size()) : declared_rows));
    for (const KinRow& r : rows) {
        body.u32(r.id);
        body.u32(r.spouse);
        body.u8(r.marriages);
        for (const std::uint32_t e : r.ex) {
            body.u32(e);
        }
        for (const std::uint32_t g : r.godparent) {
            body.u32(g);
        }
    }
    body.bytes(extra);
    ByteWriter out;
    out.bytes(std::span<const std::uint8_t>(plain.data(), plain.size() - 2));  // up to the section count
    out.u16(1);
    out.u16(id);
    out.u16(version);
    out.u32(static_cast<std::uint32_t>(body.size()));
    out.bytes(body.data());
    return out.take();
}

KinRow married(std::uint32_t a, std::uint32_t b) {
    KinRow r;
    r.id = a;
    r.spouse = b;
    r.marriages = 1;
    return r;
}

}  // namespace

TEST_CASE("kin section: a hand-made section loads to the world the API builds") {
    World expected = bare_world();
    REQUIRE(expected.marry(CharacterId(0), CharacterId(1)));
    REQUIRE(expected.marry(CharacterId(2), CharacterId(3)));
    REQUIRE(expected.end_marriage(CharacterId(2)));
    REQUIRE(expected.set_character_godparents(CharacterId(5), {CharacterId(0), CharacterId(3)}));

    KinRow r0 = married(0, 1);
    KinRow r1 = married(1, 0);
    KinRow r2;
    r2.id = 2;
    r2.marriages = 1;
    r2.ex[0] = 3;
    KinRow r3;
    r3.id = 3;
    r3.marriages = 1;
    r3.ex[0] = 2;
    KinRow r5;
    r5.id = 5;
    r5.godparent[0] = 0;
    r5.godparent[1] = 3;
    auto loaded = load(with_kin_section({r0, r1, r2, r3, r5}));
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().compute_hash() == expected.compute_hash());
    CHECK(loaded.value().characters() == expected.characters());
    CHECK(state_bytes(loaded.value()) == state_bytes(expected));  // and it writes the section back

    // the section is only present when something is stored
    CHECK(state_bytes(bare_world()).size() + 8 + 4 + 5 * 29 == state_bytes(expected).size());
}

TEST_CASE("kin section: damaged sections are errors, never crashes") {
    auto rejected = [](const std::vector<std::uint8_t>& bytes, const char* text = nullptr) {
        const auto result = load(bytes);
        if (result.has_value()) {
            return false;
        }
        return text == nullptr || result.error().message.find(text) != std::string::npos;
    };
    KinRow a = married(0, 1);
    KinRow b = married(1, 0);
    REQUIRE(load(with_kin_section({a, b})).has_value());

    CHECK(rejected(with_kin_section({a, b}, 3, 2), "unsupported"));  // another version
    CHECK(rejected(with_kin_section({a, b}, 9),
                   "unsupported"));  // another section (4 is pregnancy since schema 11)
    CHECK(rejected(with_kin_section({b, a}), "out of order"));  // ids must ascend
    CHECK(rejected(with_kin_section({a, a}), "out of order"));
    KinRow far = married(6, 0);
    CHECK(rejected(with_kin_section({a, b, far}), "out of range"));  // id >= capacity
    KinRow stray = married(0, 77);
    CHECK(rejected(with_kin_section({stray}), "outside the table"));  // reference >= capacity
    KinRow stray_ex = married(0, 1);
    stray_ex.ex[2] = 6;
    CHECK(rejected(with_kin_section({stray_ex, b}), "outside the table"));
    KinRow stray_godparent = married(0, 1);
    stray_godparent.godparent[1] = 0xFFFFFFF0U;
    CHECK(rejected(with_kin_section({stray_godparent, b}), "outside the table"));
    KinRow many = married(0, 1);
    many.marriages = 4;
    CHECK(rejected(with_kin_section({many, b}), "marriage counter"));
    CHECK(rejected(with_kin_section({a, b}, 3, 1, 3), "invalid kin section"));  // one row too many
    CHECK(rejected(with_kin_section({a, b}, 3, 1, 0x7FFFFFFF), "invalid kin section"));
    CHECK(rejected(with_kin_section({a, b}, 3, 1, -1, {0}), "malformed"));  // a stray byte
    CHECK(rejected(with_kin_section({a}), "symmetric"));                    // half a marriage
    KinRow lonely = married(0, 0);
    CHECK(rejected(with_kin_section({lonely}), "itself"));
    KinRow too_few;
    too_few.id = 0;
    too_few.ex[0] = 1;  // a former spouse but no marriage counted
    KinRow other = too_few;
    other.id = 1;
    other.ex[0] = 0;
    CHECK(rejected(with_kin_section({too_few, other}), "fewer marriages"));
    CHECK(load(with_kin_section({a, b, married(2, 3), married(3, 2), married(4, 5), married(5, 4)}))
              .has_value());  // sanity: three couples are fine
    KinRow godchild;
    godchild.id = 4;
    godchild.godparent[0] = 4;
    CHECK(rejected(with_kin_section({godchild}), "godparent"));

    // The section is not allowed in a file of schema 2, the schema word decides.
    std::vector<std::uint8_t> old_schema = with_kin_section({a, b});
    old_schema[0] = 2;
    CHECK(rejected(old_schema, "unsupported"));
    // A world flag other than dynasty mode is still an error.
    std::vector<std::uint8_t> flags = with_kin_section({a, b});
    // The flags word sits six bytes before the first section header (flags 4, count 2): find it by size.
    const std::size_t section_bytes = 8 + 4 + 2 * 29;
    flags[flags.size() - section_bytes - 6] = 3;
    CHECK(rejected(flags, "flags"));
}

TEST_CASE("kin section: random damage to the section and to the whole payload is handled safely") {
    Builder b(77);
    b.add_people(140);
    b.change_marriages(200);
    const std::vector<std::uint8_t> bytes = state_bytes(b.world);
    std::size_t rows = 0;
    for (std::size_t i = 0; i < b.world.characters().ids.capacity(); ++i) {
        const auto& t = b.world.characters();
        rows += (t.spouse[i].valid() || t.marriages[i] != 0 || t.ex_spouses[i][0].valid() ||
                 t.godparent[i][0].valid() || t.godparent[i][1].valid())
                    ? 1
                    : 0;
    }
    REQUIRE(rows > 20);
    const std::size_t section = 12 + rows * 29;  // the kin section is the last thing in the payload
    Pcg32 rng(2024, 5);
    std::size_t survived = 0;
    for (int round = 0; round < 600; ++round) {
        std::vector<std::uint8_t> damaged = bytes;
        const int flips = 1 + static_cast<int>(rng.bounded(4));
        for (int k = 0; k < flips; ++k) {
            const std::size_t where =
                round % 3 == 0 ? rng.bounded(static_cast<std::uint32_t>(damaged.size()))
                               : damaged.size() - 1 - rng.bounded(static_cast<std::uint32_t>(section));
            damaged[where] = static_cast<std::uint8_t>(rng.bounded(256));
        }
        if (round % 5 == 0) {
            damaged.resize(damaged.size() - 1 - rng.bounded(static_cast<std::uint32_t>(section)));
        }
        const auto result = load(damaged);
        if (result.has_value()) {  // an accepted file is a valid world
            ++survived;
            CHECK(result.value().check_invariants().empty());
        }
    }
    CHECK(survived < 600);  // most damage is detected
    // every prefix of a small payload is an error
    const std::vector<std::uint8_t> small = with_kin_section({married(0, 1), married(1, 0)});
    for (std::size_t n = 0; n < small.size(); n += 7) {
        CHECK_FALSE(load(std::span<const std::uint8_t>(small.data(), n)).has_value());
    }
}

// =================================================================================================
// a simulation with a system that marries people and creates children
// =================================================================================================

namespace {

/// Every few days emits a child of a deterministic couple, marries the next two unmarried adults
/// and now and then ends a marriage. All of it goes through commands.
class FamilyGrowth final : public System {
public:
    [[nodiscard]] std::string_view name() const override { return "test.family_growth"; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }
    void run(World& world, TickContext& context) override {
        const auto& t = world.characters();
        const auto living = world.living();
        if (living.size() < 4) {
            return;
        }
        const auto tick = static_cast<std::size_t>(context.tick_index);
        if (tick % 9 == 0) {
            for (const CharacterId a : living) {
                if (t.sex[a.index()] == Sex::Male && !t.spouse[a.index()].valid() &&
                    t.marriages[a.index()] < 2) {
                    for (const CharacterId b : living) {
                        if (t.sex[b.index()] == Sex::Female && !t.spouse[b.index()].valid() &&
                            t.marriages[b.index()] < 2 && !are_former_spouses(t, a, b)) {
                            context.emit(Command{DynastyId(), cmd::Marry{a, b}});
                            return;
                        }
                    }
                }
            }
        }
        if (tick % 5 == 0) {
            for (const CharacterId a : living) {
                const CharacterId b = t.spouse[a.index()];
                if (b.valid() && a < b && t.birth_date[a.index()] + 16 * 365 < context.date &&
                    t.birth_date[b.index()] + 16 * 365 < context.date && (tick + a.value()) % 3 == 0) {
                    cmd::CreatePerson child;
                    child.init.dynasty = t.dynasty[a.index()];
                    child.init.birth_date = context.date;
                    child.init.sex = tick % 2 == 0 ? Sex::Male : Sex::Female;
                    child.init.father = t.sex[a.index()] == Sex::Male ? a : b;
                    child.init.mother = t.sex[a.index()] == Sex::Male ? b : a;
                    context.emit(Command{DynastyId(), child});
                    return;
                }
            }
        }
        if (tick % 41 == 0) {
            for (const CharacterId a : living) {
                if (t.spouse[a.index()].valid() && (a.value() + tick) % 4 == 0) {
                    context.emit(Command{DynastyId(), cmd::EndMarriage{a, MarriageEnd::Annulment}});
                    return;
                }
            }
        }
    }

private:
    static bool are_former_spouses(const CharacterTable& t, CharacterId a, CharacterId b) {
        const auto& ex = t.ex_spouses[a.index()];
        return std::find(ex.begin(), ex.end(), b) != ex.end();
    }
};

Simulation family_simulation() {
    World world = dynasty_world(9, 2);
    for (int i = 0; i < 10; ++i) {
        (void)add(world,
                  i % 2 == 0 ? Sex::Male : Sex::Female,
                  year(1670 + i),
                  CharacterId(),
                  CharacterId(),
                  DynastyId(static_cast<std::uint32_t>(i % 2)));
    }
    Simulation sim(std::move(world));
    sim.add_system(std::make_unique<FamilyGrowth>());
    return sim;
}

}  // namespace

TEST_CASE("simulation: marriages and births through commands, save in the middle, replay") {
    Simulation original = family_simulation();
    original.run_days(400);
    require_valid(original.world());
    CHECK(original.world().living().size() > 14);
    CHECK(original.world().compute_hash().ext[ext_index(ExtPart::Kinship)] != 0);
    std::size_t children = 0;
    for (const CharacterId id : original.world().living()) {
        children += kin::child_count(original.world(), id);
    }
    CHECK(children > 4);

    // save in the middle (a command may be pending) and resume
    const auto bytes = serialize_save(original.world(), original.pending_internal());
    Simulation resumed = family_simulation();
    auto loaded = deserialize_save(bytes);
    REQUIRE(loaded.has_value());
    resumed.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
    CHECK(resumed.world().compute_hash() == original.world().compute_hash());
    original.run_days(500);
    resumed.run_days(500);
    CHECK(resumed.world().compute_hash() == original.world().compute_hash());
    CHECK(resumed.world().characters() == original.world().characters());
    require_valid(original.world());
    require_valid(resumed.world());

    // a replay with an external Marry command reproduces the run, kinship part included
    Simulation recorded = family_simulation();
    ReplayRecorder recorder(9, 0x4B1DULL, recorded.world().today(), 30);
    for (int day = 0; day < 200; ++day) {
        if (day == 3) {
            REQUIRE(recorded.submit(Command{DynastyId(), cmd::Marry{CharacterId(8), CharacterId(5)}}));
        }
        (void)recorded.tick();
        recorder.after_tick(recorded);
    }
    const Replay replay = recorder.finish(recorded);
    auto parsed = deserialize_replay(serialize_replay(replay));
    REQUIRE(parsed.has_value());
    Simulation again = family_simulation();
    const ReplayResult result = play_replay(again, parsed.value());
    CHECK(result.ok);
    CHECK(again.world().compute_hash() == recorded.world().compute_hash());
    CHECK(again.world().characters() == recorded.world().characters());
}
