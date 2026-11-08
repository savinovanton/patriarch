// Death and the archive (E7-T04, ADR-0006 D3/D6): the command Die and World::die, the archived record,
// widowhood, the head of the dynasty, the queue, the hash part of the archive, the journal of the tick,
// save/load/replay with an archive (state schema 4), damaged archive data, and a property test against an
// independent model.

#include "pat/core/bytes.hpp"
#include "pat/core/compress.hpp"
#include "pat/core/hash_stream.hpp"
#include "pat/core/rng.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/command_log.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/kinship.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/scheduled_queue.hpp"
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
#include <string_view>
#include <vector>

using namespace pat::sim;
using pat::core::ByteReader;
using pat::core::ByteWriter;
using pat::core::Fixed32;
using pat::core::Pcg32;
using pat::core::StringId;

namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFU;

Fixed32 fx(int value) {
    return Fixed32::from_int(value);
}

constexpr std::size_t ext_index(ExtPart part) {
    return static_cast<std::size_t>(part);
}

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

/// Tests corrupt a World on purpose, which the API does not allow: the columns are const.
CharacterTable& table_of(World& world) {
    return const_cast<CharacterTable&>(world.characters());
}

void set_slot_state(World& world, CharacterId id, std::uint8_t state) {
    const auto flags = world.characters().ids.alive_flags();
    const_cast<std::uint8_t*>(flags.data())[id.index()] = state;
}

Date year(int y, int month = 1, int day = 1) {
    return Date::from_ymd(y, month, day);
}

World dynasty_world(std::uint64_t seed = 3, std::uint32_t dynasties = 2) {
    World world(year(1703), seed);
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

/// Two houses, three generations. The ids are in the order of the fields.
struct Family {
    World world = dynasty_world();
    CharacterId grandfather, grandmother, father, mother, son, daughter, uncle;
    BranchId branch;

    Family() {
        grandfather = add(world, Sex::Male, year(1620));
        grandmother = add(world, Sex::Female, year(1625));
        father = add(world, Sex::Male, year(1650), grandfather, grandmother);
        uncle = add(world, Sex::Male, year(1652), grandfather, grandmother);
        mother = add(world, Sex::Female, year(1655), CharacterId(), CharacterId(), DynastyId(1));
        son = add(world, Sex::Male, year(1680), father, mother);
        daughter = add(world, Sex::Female, year(1682), father, mother);
        REQUIRE(world.marry(grandfather, grandmother));
        REQUIRE(world.marry(father, mother));
        branch = world.create_branch(DynastyId(0), StringId(77));
        REQUIRE(world.set_character_branch(father, branch));
        world.set_character_name(father, StringId(500));
        world.set_character_portrait(father, 9);
        world.set_character_legitimacy(father, Legitimacy::Legitimized);
        world.set_character_skill(father, Skill::Martial, fx(12));
        world.set_character_fertility(father, fx(40));
        world.set_character_stress(father, fx(25));
        world.set_character_piety(father, fx(55));
        world.set_character_ambition(father, fx(-30));
        world.set_character_loyalty_head(father, fx(20));
        world.set_character_loyalty_emperor(father, fx(-5));
        const std::vector<std::uint16_t> traits = {4, 9};
        REQUIRE(world.set_character_traits(father, traits));
        REQUIRE(world.set_character_education(father, Education{3, grandfather, 40}));
        REQUIRE(world.set_character_planned_death(father, year(1730)));
        REQUIRE(world.set_dynasty_head(DynastyId(0), father));
        require_valid(world);
    }
};

std::size_t queue_entries_of(const World& world, CharacterId who) {
    std::size_t n = 0;
    for (const auto& entry : world.life_queue().snapshot()) {
        n += entry.item.character == who ? 1 : 0;
    }
    return n;
}

bool contains(std::span<const CharacterId> ids, CharacterId who) {
    return std::find(ids.begin(), ids.end(), who) != ids.end();
}

bool contains(const std::vector<CharacterId>& ids, CharacterId who) {
    return std::find(ids.begin(), ids.end(), who) != ids.end();
}

}  // namespace

// =================================================================================================
// the queue helper
// =================================================================================================

TEST_CASE("scheduled queue: remove_if drops the matching entries and keeps the order of the rest") {
    ScheduledQueue<int> queue;
    for (int i = 0; i < 20; ++i) {
        queue.push(year(1703) + (i * 7) % 11, i);
    }
    CHECK(queue.remove_if([](Date, int item) { return item % 3 == 0; }) == 7);
    CHECK(queue.remove_if([](Date, int) { return false; }) == 0);
    std::vector<int> order;
    std::vector<int> expected;
    for (const auto& entry : queue.snapshot()) {
        expected.push_back(entry.item);
    }
    queue.pop_due(year(1704), [&](Date, int item) { order.push_back(item); });
    CHECK(order == expected);
    CHECK(order.size() == 13);
    CHECK(std::none_of(order.begin(), order.end(), [](int item) { return item % 3 == 0; }));
}

// =================================================================================================
// World::die
// =================================================================================================

TEST_CASE("die: the dead leave every index and keep the facts of the family") {
    Family f;
    World& w = f.world;
    const WorldHash before = w.compute_hash();
    CHECK(before.ext[ext_index(ExtPart::Archive)] == 0);
    CHECK(w.archived_count() == 0);
    const std::size_t living_before = w.living().size();
    const std::size_t queue_before = w.life_queue().size();
    CHECK(queue_entries_of(w, f.father) == 2);  // birthday and planned death
    CHECK(w.characters().ids.capacity() == 7);

    w.advance_day();
    REQUIRE(w.die(f.father, CauseOfDeath::Illness));
    require_valid(w);

    // not alive, but the row exists and sits in the archive
    CHECK_FALSE(w.is_alive(f.father));
    CHECK(w.exists(f.father));
    CHECK(w.is_archived(f.father));
    CHECK(w.archived_count() == 1);
    CHECK(w.characters().ids.retired_count() == 1);
    CHECK(w.living().size() == living_before - 1);
    CHECK_FALSE(contains(w.living(), f.father));
    CHECK_FALSE(contains(w.members_of(DynastyId(0)), f.father));
    CHECK(w.members_of(DynastyId(0)).size() == 5);
    CHECK_FALSE(w.dynasties().head[0].valid());  // the successor is chosen by E7-T15
    CHECK(queue_entries_of(w, f.father) == 0);
    CHECK(w.life_queue().size() == queue_before - 2);

    // the facts of the death and of the identity
    const CharacterTable& t = w.characters();
    CHECK(t.death_date[f.father.index()] == w.today());
    CHECK(t.cause_of_death[f.father.index()] == static_cast<std::uint8_t>(CauseOfDeath::Illness));
    CHECK(t.name[f.father.index()] == StringId(500));
    CHECK(t.branch[f.father.index()] == f.branch);
    CHECK(t.portrait[f.father.index()] == 9);
    CHECK(t.legitimacy[f.father.index()] == Legitimacy::Legitimized);
    CHECK(t.dynasty[f.father.index()] == DynastyId(0));
    CHECK(t.birth_date[f.father.index()] == year(1650));

    // the state of the living is gone
    CHECK(t.health[f.father.index()] == Fixed32::zero());
    CHECK(t.skills[f.father.index()] == std::array<Fixed32, kSkillCount>{});
    CHECK(t.fertility[f.father.index()] == Fixed32::zero());
    CHECK(t.stress[f.father.index()] == Fixed32::zero());
    CHECK(t.piety[f.father.index()] == Fixed32::zero());
    CHECK(t.ambition[f.father.index()] == Fixed32::zero());
    CHECK(t.loyalty_head[f.father.index()] == Fixed32::zero());
    CHECK(t.loyalty_emperor[f.father.index()] == Fixed32::zero());
    CHECK(t.traits[f.father.index()] == std::array<std::uint16_t, kTraitSlots>{});
    CHECK(t.education_focus[f.father.index()] == 0);
    CHECK_FALSE(t.education_tutor[f.father.index()].valid());
    CHECK(t.education_level[f.father.index()] == 0);
    CHECK(t.planned_death[f.father.index()] == kNoDate);

    // the family tree is whole
    CHECK(kin::children_of(w, f.father) == std::vector<CharacterId>{f.son, f.daughter});
    CHECK(kin::child_count(w, f.father) == 2);
    CHECK(kin::siblings_of(w, f.son) == std::vector<CharacterId>{f.daughter});
    CHECK(kin::siblings_of(w, f.father) == std::vector<CharacterId>{f.uncle});
    CHECK(contains(kin::ancestors_of(w, f.son), f.father));
    CHECK(contains(kin::ancestors_of(w, f.son), f.grandfather));
    CHECK(contains(kin::descendants_of(w, f.grandfather), f.father));
    CHECK(kin::is_ancestor_of(w, f.father, f.daughter));
    CHECK(kin::ancestors_of(w, f.father) == std::vector<CharacterId>{f.grandfather, f.grandmother});
    CHECK(w.characters().father[f.son.index()] == f.father);

    // widowhood
    CHECK_FALSE(t.spouse[f.mother.index()].valid());
    CHECK(t.ex_spouses[f.mother.index()][0] == f.father);
    CHECK(t.marriages[f.mother.index()] == 1);
    CHECK_FALSE(t.spouse[f.father.index()].valid());
    CHECK(t.ex_spouses[f.father.index()][0] == f.mother);
    CHECK(t.marriages[f.father.index()] == 1);

    // the record of the archive is a copy of the facts
    const auto person = w.archived_person(f.father);
    REQUIRE(person.has_value());
    CHECK(person->id == f.father);
    CHECK(person->dynasty == DynastyId(0));
    CHECK(person->branch == f.branch);
    CHECK(person->name == StringId(500));
    CHECK(person->portrait == 9);
    CHECK(person->sex == Sex::Male);
    CHECK(person->legitimacy == Legitimacy::Legitimized);
    CHECK(person->birth_date == year(1650));
    CHECK(person->death_date == w.today());
    CHECK(person->cause == CauseOfDeath::Illness);
    CHECK(person->father == f.grandfather);
    CHECK(person->mother == f.grandmother);
    CHECK(person->marriages == 1);
    CHECK(person->ex_spouses[0] == f.mother);
    CHECK_FALSE(person->ex_spouses[1].valid());
    CHECK(person->children == std::vector<CharacterId>{f.son, f.daughter});
    CHECK_FALSE(w.archived_person(f.mother).has_value());         // alive
    CHECK_FALSE(w.archived_person(CharacterId(99)).has_value());  // unknown
    CHECK_FALSE(w.archived_person(CharacterId()).has_value());

    // an id is never handed out again
    const CharacterId newcomer = add(w, Sex::Female, year(1700));
    CHECK(newcomer.index() == 7);
    CHECK(newcomer != f.father);
    CHECK(w.characters().ids.capacity() == 8);
    CHECK(w.characters().ids.free_list().empty());
    require_valid(w);

    // the archive is part of the hash
    const WorldHash after = w.compute_hash();
    CHECK(after.ext[ext_index(ExtPart::Archive)] != 0);
    CHECK(after.total != before.total);
    CHECK(after.characters != before.characters);
}

TEST_CASE("die: members of a house are swap-removed like the old sparse set did") {
    Family f;
    World& w = f.world;
    const std::vector<CharacterId> start(w.members_of(DynastyId(0)).begin(),
                                         w.members_of(DynastyId(0)).end());
    REQUIRE(start ==
            std::vector<CharacterId>{f.grandfather, f.grandmother, f.father, f.uncle, f.son, f.daughter});
    REQUIRE(w.die(f.grandfather, CauseOfDeath::OldAge));
    const std::vector<CharacterId> now(w.members_of(DynastyId(0)).begin(), w.members_of(DynastyId(0)).end());
    CHECK(now == std::vector<CharacterId>{
                     f.daughter, f.grandmother, f.father, f.uncle, f.son});  // the last one took its place
    require_valid(w);
    REQUIRE(w.die(f.father, CauseOfDeath::Wounds));
    const std::vector<CharacterId> later(w.members_of(DynastyId(0)).begin(),
                                         w.members_of(DynastyId(0)).end());
    CHECK(later == std::vector<CharacterId>{f.daughter, f.grandmother, f.son, f.uncle});
    require_valid(w);
}

TEST_CASE("die: only a living person of dynasty mode dies, once, with a known cause") {
    Family f;
    World& w = f.world;
    const WorldHash before = w.compute_hash();
    CHECK_FALSE(w.die(f.father, CauseOfDeath::None));
    CHECK_FALSE(w.die(f.father, static_cast<CauseOfDeath>(kCauseOfDeathCount)));
    CHECK_FALSE(w.die(f.father, static_cast<CauseOfDeath>(200)));
    CHECK_FALSE(w.die(CharacterId(), CauseOfDeath::Grief));
    CHECK_FALSE(w.die(CharacterId(99), CauseOfDeath::Grief));
    CHECK(w.compute_hash() == before);

    REQUIRE(w.die(f.father, CauseOfDeath::Murder));
    const WorldHash dead = w.compute_hash();
    CHECK_FALSE(w.die(f.father, CauseOfDeath::OldAge));  // already dead: nothing changes
    CHECK(w.compute_hash() == dead);
    CHECK(w.characters().cause_of_death[f.father.index()] == static_cast<std::uint8_t>(CauseOfDeath::Murder));

    // somebody who is not born yet cannot die
    const CharacterId unborn = add(w, Sex::Male, w.today() + 10);
    CHECK_FALSE(w.die(unborn, CauseOfDeath::Accident));
    CHECK(w.is_alive(unborn));

    World e1(year(1703), 1);
    const DynastyId d = e1.create_dynasty(StringId(0));
    CharacterInit init;
    init.dynasty = d;
    init.birth_date = year(1680);
    const CharacterId c = e1.create_character(init);
    CHECK_FALSE(e1.die(c, CauseOfDeath::OldAge));  // E1 mode destroys (KillCharacter), it does not archive
    CHECK(e1.is_alive(c));
}

TEST_CASE("die: every cause is stored and listed in the archive") {
    World w = dynasty_world();
    for (std::uint8_t cause = 1; cause < kCauseOfDeathCount; ++cause) {
        const CharacterId id = add(w, Sex::Male, year(1650));
        REQUIRE(w.die(id, static_cast<CauseOfDeath>(cause)));
        CHECK(w.archived_person(id)->cause == static_cast<CauseOfDeath>(cause));
        CHECK(w.characters().cause_of_death[id.index()] == cause);
    }
    CHECK(w.archived_count() == 9);
    CHECK(w.living().empty());
    require_valid(w);
}

TEST_CASE("die: a widow can marry again and the dead stay in the family") {
    Family f;
    World& w = f.world;
    REQUIRE(w.die(f.father, CauseOfDeath::Illness));
    CHECK_FALSE(w.marry(f.mother, f.father));  // the dead do not marry

    const CharacterId suitor = add(w, Sex::Male, year(1650), CharacterId(), CharacterId(), DynastyId(1));
    REQUIRE(w.marry(f.mother, suitor));
    CHECK(w.characters().marriages[f.mother.index()] == 2);
    CHECK(w.characters().spouse[f.mother.index()] == suitor);
    REQUIRE(w.die(suitor, CauseOfDeath::War));
    const CharacterTable& t = w.characters();
    CHECK(t.ex_spouses[f.mother.index()][0] == f.father);
    CHECK(t.ex_spouses[f.mother.index()][1] == suitor);
    CHECK_FALSE(t.spouse[f.mother.index()].valid());
    CHECK(w.archived_person(suitor)->ex_spouses[0] == f.mother);
    require_valid(w);

    // both partners of a marriage die, one after the other
    REQUIRE(w.die(f.grandfather, CauseOfDeath::OldAge));
    REQUIRE(w.die(f.grandmother, CauseOfDeath::OldAge));
    CHECK(t.ex_spouses[f.grandfather.index()][0] == f.grandmother);
    CHECK(t.ex_spouses[f.grandmother.index()][0] == f.grandfather);
    CHECK(w.archived_person(f.grandmother)->marriages == 1);
    require_valid(w);
}

TEST_CASE("die: the head of the house is reset only when the head dies") {
    Family f;
    World& w = f.world;
    REQUIRE(w.set_dynasty_head(DynastyId(1), f.mother));
    REQUIRE(w.die(f.uncle, CauseOfDeath::Accident));
    CHECK(w.dynasties().head[0] == f.father);  // somebody else died
    REQUIRE(w.die(f.mother, CauseOfDeath::Childbirth));
    CHECK_FALSE(w.dynasties().head[1].valid());
    CHECK(w.dynasties().head[0] == f.father);
    require_valid(w);
    CHECK_FALSE(w.set_dynasty_head(DynastyId(1), f.mother));  // a dead person cannot be made head
    CHECK(w.dynasties().head[1] == CharacterId());
}

TEST_CASE("die: references of the living to the dead are tolerated, the dead get no new data") {
    Family f;
    World& w = f.world;
    REQUIRE(w.set_character_education(f.son, Education{2, f.father, 10}));  // the father is the tutor
    REQUIRE(w.die(f.father, CauseOfDeath::Illness));
    CHECK(w.characters().education_tutor[f.son.index()] == f.father);  // lazy cleaning, ADR-0006 D1
    require_valid(w);

    // the setters ignore the dead
    const WorldHash frozen = w.compute_hash();
    w.set_character_name(f.father, StringId(1));
    w.set_character_portrait(f.father, 5);
    w.set_character_stress(f.father, fx(50));
    CHECK_FALSE(w.set_character_branch(f.father, f.branch));
    CHECK_FALSE(w.set_character_education(f.father, Education{}));
    CHECK_FALSE(w.set_character_planned_death(f.father, year(1740)));
    CHECK_FALSE(w.end_marriage(f.father));
    CHECK_FALSE(w.set_character_godparents(f.father, {f.uncle, CharacterId()}));
    CHECK(w.compute_hash() == frozen);
    require_valid(w);
}

TEST_CASE("die: a child born after the death of the father is still in his list") {
    Family f;
    World& w = f.world;
    REQUIRE(w.die(f.father, CauseOfDeath::War));
    const CharacterId posthumous = add(w, Sex::Male, w.today(), f.father, f.mother);
    CHECK(w.characters().father[posthumous.index()] == f.father);
    CHECK(kin::children_of(w, f.father) == std::vector<CharacterId>{f.son, f.daughter, posthumous});
    CHECK(w.archived_person(f.father)->children.size() == 3);
    require_valid(w);
    REQUIRE(w.die(posthumous, CauseOfDeath::Illness));  // an archived child of an archived father
    CHECK(kin::children_of(w, f.father).size() == 3);
    require_valid(w);
}

// =================================================================================================
// the hash part of the archive
// =================================================================================================

TEST_CASE("hash: the archive part is empty without the dead and independent of the order of death") {
    const auto build = [](bool backwards) {
        Family f;
        if (backwards) {
            REQUIRE(f.world.die(f.uncle, CauseOfDeath::Grief));
            REQUIRE(f.world.die(f.son, CauseOfDeath::Illness));
        } else {
            REQUIRE(f.world.die(f.son, CauseOfDeath::Illness));
            REQUIRE(f.world.die(f.uncle, CauseOfDeath::Grief));
        }
        return f.world.compute_hash();
    };
    const WorldHash forward = build(false);
    const WorldHash backward = build(true);
    CHECK(forward.ext[ext_index(ExtPart::Archive)] != 0);
    CHECK(forward.ext[ext_index(ExtPart::Archive)] == backward.ext[ext_index(ExtPart::Archive)]);
    // (the lists of members differ in order, so the classic part `dynasties` may differ, and does)
    Family f;
    CHECK(f.world.compute_hash().ext[ext_index(ExtPart::Archive)] == 0);

    // the cause and the date are part of the row
    Family g;
    Family h;
    REQUIRE(g.world.die(g.son, CauseOfDeath::Illness));
    REQUIRE(h.world.die(h.son, CauseOfDeath::Wounds));
    CHECK(g.world.compute_hash().ext[ext_index(ExtPart::Archive)] !=
          h.world.compute_hash().ext[ext_index(ExtPart::Archive)]);
    Family i;
    i.world.advance_day();
    REQUIRE(i.world.die(i.son, CauseOfDeath::Illness));
    CHECK(g.world.compute_hash().ext[ext_index(ExtPart::Archive)] !=
          i.world.compute_hash().ext[ext_index(ExtPart::Archive)]);
}

TEST_CASE("hash: total absorbs the archive part only when there are dead, the demo is untouched") {
    Family f;
    const WorldHash alive = f.world.compute_hash();
    REQUIRE(f.world.die(f.uncle, CauseOfDeath::Accident));
    WorldHash dead = f.world.compute_hash();
    CHECK(dead.total != alive.total);

    // recompute total from the parts: five classic parts, then the used extended parts in order
    pat::core::HashStream total;
    total.u64(dead.calendar);
    total.u64(dead.rng);
    total.u64(dead.dynasties);
    total.u64(dead.provinces);
    total.u64(dead.characters);
    for (std::size_t part = 0; part < kExtPartCount; ++part) {
        if (dead.ext[part] != 0) {
            total.u8(static_cast<std::uint8_t>(part));
            total.u64(dead.ext[part]);
        }
    }
    CHECK(total.value() == dead.total);
    CHECK(ext_part_name(ext_index(ExtPart::Archive)) == "archive");

    // E1 mode never has an archive: the demo world has an empty part
    World demo = demo::make_world(demo::Config{});
    CHECK(demo.compute_hash().ext[ext_index(ExtPart::Archive)] == 0);
}

// =================================================================================================
// the command Die
// =================================================================================================

TEST_CASE("Die: validate gives a reason for everything it refuses") {
    Family f;
    World& w = f.world;
    const auto verdict = [&](const Command& c) { return validate(w, c); };
    const auto reason_of = [&](const Command& c) {
        const auto v = validate(w, c);
        REQUIRE_FALSE(v.has_value());
        return v.error();
    };

    CHECK(verdict(Command{DynastyId(), cmd::Die{f.father, CauseOfDeath::OldAge}}).has_value());
    CHECK(reason_of(Command{DynastyId(0), cmd::Die{f.father, CauseOfDeath::OldAge}}).reason ==
          Rejection::NotAuthorized);
    CHECK(reason_of(Command{DynastyId(), cmd::Die{CharacterId(99), CauseOfDeath::OldAge}}).reason ==
          Rejection::UnknownEntity);
    CHECK(reason_of(Command{DynastyId(), cmd::Die{CharacterId(), CauseOfDeath::OldAge}}).reason ==
          Rejection::UnknownEntity);
    CHECK(reason_of(Command{DynastyId(), cmd::Die{f.father, CauseOfDeath::None}}).reason ==
          Rejection::InvalidArgument);
    CHECK(reason_of(Command{DynastyId(), cmd::Die{f.father, static_cast<CauseOfDeath>(77)}}).reason ==
          Rejection::InvalidArgument);
    const CharacterId unborn = add(w, Sex::Female, w.today() + 5);
    CHECK(reason_of(Command{DynastyId(), cmd::Die{unborn, CauseOfDeath::Illness}}).reason ==
          Rejection::InvalidState);

    // the second death of the same person
    apply(w, Command{DynastyId(), cmd::Die{f.father, CauseOfDeath::Illness}});
    const CommandError again = reason_of(Command{DynastyId(), cmd::Die{f.father, CauseOfDeath::Illness}});
    CHECK(again.reason == Rejection::InvalidState);
    CHECK(std::string_view(again.detail).find("already dead") != std::string_view::npos);

    // KillCharacter belongs to E1 mode; Die belongs to dynasty mode
    const CommandError kill = reason_of(Command{DynastyId(), cmd::KillCharacter{f.mother}});
    CHECK(kill.reason == Rejection::InvalidState);
    CHECK(std::string_view(kill.detail).find("Die") != std::string_view::npos);
    World e1(year(1703), 1);
    const DynastyId d = e1.create_dynasty(StringId(0));
    CharacterInit init;
    init.dynasty = d;
    init.birth_date = year(1680);
    const CharacterId c = e1.create_character(init);
    CHECK(validate(e1, Command{DynastyId(), cmd::Die{c, CauseOfDeath::OldAge}}).error().reason ==
          Rejection::InvalidState);
    CHECK(validate(e1, Command{DynastyId(), cmd::KillCharacter{c}}).has_value());
}

TEST_CASE("Die: applying the command archives the person; the journal shows what happened") {
    Family f;
    World& w = f.world;
    CommandLog log;
    const std::vector<Command> batch = {
        Command{DynastyId(), cmd::Die{f.father, CauseOfDeath::Wounds}},
        Command{DynastyId(), cmd::Die{f.father, CauseOfDeath::Wounds}},  // the second one is refused
        Command{DynastyId(), cmd::Die{f.uncle, CauseOfDeath::Illness}},
    };
    const ApplyStats stats = process_commands(w, batch, CommandSource::Internal, log);
    CHECK(stats.applied == 2);
    CHECK(stats.rejected == 1);
    REQUIRE(log.size() == 3);
    CHECK(log.entries()[0].accepted());
    CHECK(log.entries()[1].outcome == Rejection::InvalidState);
    CHECK(log.entries()[2].accepted());
    CHECK(type_of(log.entries()[0].command) == CommandType::Die);
    CHECK(to_string(CommandType::Die) == "Die");
    CHECK(w.is_archived(f.father));
    CHECK(w.is_archived(f.uncle));
    CHECK(w.archived_person(f.uncle)->cause == CauseOfDeath::Illness);
    require_valid(w);

    // The journal survives its own serialization, so E10/E14/E17 can read it from a file too
    auto back = CommandLog::deserialize(log.serialize());
    REQUIRE(back.has_value());
    CHECK(back.value() == log);
}

TEST_CASE("Die: the record in the command log is exact and any shorter record is an error") {
    const Command command{DynastyId(), cmd::Die{CharacterId(4), CauseOfDeath::Grief}};
    ByteWriter w;
    write_command(w, command);
    CHECK(w.data()[0] == 6);
    CHECK(w.data().size() == 1 + 4 + 4 + 1);
    ByteReader in(w.data());
    Command back;
    REQUIRE(read_command(in, back));
    CHECK(back == command);
    // an unknown cause is damage
    std::vector<std::uint8_t> bytes(w.data().begin(), w.data().end());
    bytes.back() = kCauseOfDeathCount;
    ByteReader bad(bytes);
    Command ignored;
    CHECK_FALSE(read_command(bad, ignored));
    bytes.back() = 0;  // None is a legal byte, validate() refuses the command
    ByteReader none(bytes);
    CHECK(read_command(none, ignored));
}

// =================================================================================================
// save and load
// =================================================================================================

TEST_CASE("save: a world with an archive round-trips, re-saves to the same bytes, schema 4") {
    Family f;
    World& w = f.world;
    w.advance_day();
    REQUIRE(w.die(f.father, CauseOfDeath::Illness));
    REQUIRE(w.die(f.grandmother, CauseOfDeath::OldAge));
    const std::vector<std::uint8_t> bytes = state_bytes(w);
    ByteReader schema(std::span<const std::uint8_t>(bytes.data(), 4));
    CHECK(schema.u32() == 4);
    CHECK(World::kStateSchema == 4);

    auto loaded = load(bytes);
    REQUIRE(loaded.has_value());
    World& copy = loaded.value();
    require_valid(copy);
    CHECK(copy.compute_hash() == w.compute_hash());
    CHECK(copy.characters() == w.characters());
    CHECK(state_bytes(copy) == bytes);
    CHECK(copy.archived_count() == 2);
    CHECK(copy.is_archived(f.father));
    CHECK(copy.archived_person(f.father) == w.archived_person(f.father));
    CHECK(copy.archived_person(f.grandmother) == w.archived_person(f.grandmother));
    CHECK(copy.life_queue().size() == w.life_queue().size());
    CHECK(std::equal(copy.members_of(DynastyId(0)).begin(),
                     copy.members_of(DynastyId(0)).end(),
                     w.members_of(DynastyId(0)).begin(),
                     w.members_of(DynastyId(0)).end()));
    CHECK(kin::children_of(copy, f.father) == kin::children_of(w, f.father));

    // the loaded world goes on exactly like the original: more deaths, new people, same ids
    for (World* world : {&w, &copy}) {
        REQUIRE(world->die(f.uncle, CauseOfDeath::Accident));
        const CharacterId baby = add(*world, Sex::Male, world->today(), f.grandfather, CharacterId());
        CHECK(baby.index() == 7);
        REQUIRE(world->die(f.mother, CauseOfDeath::Childbirth));
    }
    CHECK(copy.compute_hash() == w.compute_hash());
    CHECK(copy.characters() == w.characters());
    CHECK(state_bytes(copy) == state_bytes(w));
    require_valid(copy);
}

TEST_CASE("save: the demo world saves the same layout as before and has no archive") {
    World demo = demo::make_world(demo::Config{});
    const std::vector<std::uint8_t> bytes = state_bytes(demo);
    auto loaded = load(bytes);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().compute_hash() == demo.compute_hash());
    CHECK(loaded.value().archived_count() == 0);
}

TEST_CASE("migration: the schema-3 dynasty-mode fixture loads, keeps its hash and can be given a death") {
    // Made by the code of E7-T03 (state schema 3), before any change of E7-T04:
    //   stressgen --profile year1 --seed 7 --characters 240 --dynasties 8 --provinces 24
    //             --armies 0 --schemes 0 --contracts 0 --events 0 --output dynasty_v3.psv
    const auto file = read_file(fixture_path("dynasty_v3.psv"));
    REQUIRE(file.has_value());
    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());
    World& world = loaded.value().world;
    require_valid(world);
    const WorldHash hash = world.compute_hash();
    CHECK(hash.total == 0xd11a46b2f4fc2641ULL);
    CHECK(world.dynasty_mode());
    CHECK(world.living().size() == 240);
    CHECK(world.archived_count() == 0);
    CHECK(hash.ext[ext_index(ExtPart::Archive)] == 0);
    CHECK(hash.ext[ext_index(ExtPart::Kinship)] != 0);

    // The old members lists keep their order in the new member index.
    std::size_t members = 0;
    for (std::uint32_t d = 0; d < world.dynasties().size(); ++d) {
        members += world.members_of(DynastyId(d)).size();
    }
    CHECK(members == 240);

    // It re-saves to the same bytes except for the schema word.
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
    CHECK(old_schema.u32() == 3);
    const std::vector<std::uint8_t> now = state_bytes(world);
    REQUIRE(now.size() <= old_bytes.size());
    CHECK(std::equal(now.begin() + 4, now.end(), old_bytes.begin() + 4));
    CHECK(old_bytes.size() == now.size() + 4);  // (the count of pending commands follows the world)

    // And it can be played on: kill a few, save, load, same state.
    const std::vector<CharacterId> living(world.living().begin(), world.living().end());
    for (std::size_t i = 0; i < living.size(); i += 5) {
        REQUIRE(world.die(living[i], static_cast<CauseOfDeath>(1 + i % 9)));
    }
    CHECK(world.archived_count() == 48);
    require_valid(world);
    auto again = load(state_bytes(world));
    REQUIRE(again.has_value());
    CHECK(again.value().compute_hash() == world.compute_hash());
    require_valid(again.value());
}

TEST_CASE("migration: a file that says schema 3 cannot hold an archived row") {
    Family f;
    REQUIRE(f.world.die(f.father, CauseOfDeath::Illness));
    std::vector<std::uint8_t> bytes = state_bytes(f.world);
    REQUIRE(load(bytes).has_value());
    bytes[0] = 3;  // the schema word, little endian
    const auto old = load(bytes);
    REQUIRE_FALSE(old.has_value());
    CHECK(std::string_view(old.error().message).find("schema 4") != std::string_view::npos);
    bytes[0] = 5;  // newer than this build
    CHECK_FALSE(load(bytes).has_value());
}

// =================================================================================================
// damaged archive
// =================================================================================================

namespace {

/// A world with a living and a dead part for the damage tests.
World world_with_archive() {
    Family f;
    World w = std::move(f.world);
    REQUIRE(w.die(f.father, CauseOfDeath::Illness));
    REQUIRE(w.die(f.grandfather, CauseOfDeath::OldAge));
    return w;
}

}  // namespace

TEST_CASE("invariants: a damaged archived row is reported and refused by the loader") {
    const auto expect_bad = [](World w, const char* text) {
        CAPTURE(text);
        const auto problems = w.check_invariants();
        CHECK(mentions(problems, text));
        CHECK_FALSE(load(state_bytes(w)).has_value());
    };
    const CharacterId dead(2);  // the father
    {
        World w = world_with_archive();
        REQUIRE(w.is_archived(dead));
        require_valid(w);
        table_of(w).cause_of_death[dead.index()] = 0;
        expect_bad(std::move(w), "no known cause of death");
    }
    {
        World w = world_with_archive();
        table_of(w).cause_of_death[dead.index()] = kCauseOfDeathCount;
        expect_bad(std::move(w), "no known cause of death");
    }
    {
        World w = world_with_archive();
        table_of(w).death_date[dead.index()] = kNoDate;
        expect_bad(std::move(w), "no plausible death date");
    }
    {
        World w = world_with_archive();
        table_of(w).death_date[dead.index()] = year(1600);  // before the birth
        expect_bad(std::move(w), "no plausible death date");
    }
    {
        World w = world_with_archive();
        table_of(w).planned_death[dead.index()] = year(1760);
        expect_bad(std::move(w), "state of a living person");
    }
    {
        World w = world_with_archive();
        table_of(w).health[dead.index()] = fx(50);
        expect_bad(std::move(w), "state of a living person");
    }
    {
        World w = world_with_archive();
        table_of(w).traits[dead.index()][0] = 3;
        expect_bad(std::move(w), "state of a living person");
    }
    {
        World w = world_with_archive();
        table_of(w).education_tutor[dead.index()] = CharacterId(0);
        expect_bad(std::move(w), "state of a living person");
    }
    {
        World w = world_with_archive();
        table_of(w).spouse[dead.index()] = CharacterId(4);  // still married to the widow
        expect_bad(std::move(w), "still married");
    }
    {
        World w = world_with_archive();
        table_of(w).ex_spouses[dead.index()] = {};  // the widow remembers the marriage, the dead does not
        expect_bad(std::move(w), "former marriage");
    }
    {
        World w = world_with_archive();
        table_of(w).father[dead.index()] = dead;  // its own father
        expect_bad(std::move(w), "own parent");
    }
    {
        World w = world_with_archive();
        table_of(w).branch[dead.index()] = BranchId(5);
        expect_bad(std::move(w), "unknown branch");
    }
    {
        // the running hash notices a change of a frozen row
        World w = world_with_archive();
        table_of(w).name[dead.index()] = StringId(1234);
        const auto problems = w.check_invariants();
        CHECK(mentions(problems, "running hash of the archive"));
    }
    {
        // a dead person that was never removed from the member list: a living slot flipped to archived
        World w = world_with_archive();
        const CharacterId member(3);  // the uncle, still a member of dynasty 0
        REQUIRE(w.is_alive(member));
        table_of(w).death_date[member.index()] = year(1710);
        table_of(w).cause_of_death[member.index()] = 1;
        table_of(w).health[member.index()] = Fixed32::zero();
        table_of(w).skills[member.index()] = {};
        table_of(w).planned_death[member.index()] = kNoDate;
        set_slot_state(w, member, 2);
        CHECK_FALSE(load(state_bytes(w)).has_value());
    }
    {
        // a living slot flipped to archived without any facts of a death
        World w = world_with_archive();
        set_slot_state(w, CharacterId(3), 2);
        CHECK_FALSE(load(state_bytes(w)).has_value());
    }
    {
        // an archived row in E1 mode (a person without a house, so that no member list gives it away)
        World e1(year(1703), 1);
        CharacterInit init;
        init.birth_date = year(1680);
        const CharacterId c = e1.create_character(init);
        table_of(e1).death_date[c.index()] = year(1700);
        table_of(e1).cause_of_death[c.index()] = 1;
        table_of(e1).health[c.index()] = Fixed32::zero();
        set_slot_state(e1, c, 2);
        const auto refused = load(state_bytes(e1));
        REQUIRE_FALSE(refused.has_value());
        CHECK(std::string_view(refused.error().message).find("need dynasty mode") != std::string_view::npos);
    }
}

TEST_CASE("invariants: a queue entry of a dead person is a defect, and so is a member list entry") {
    // The queue is derived and rebuilt on load, so a damaged file cannot carry such an entry; the
    // check guards the running world. We simulate a defect by archiving the row without the cleanup.
    World w = world_with_archive();
    require_valid(w);
    const auto flags_before = w.characters().ids.alive_flags()[3];
    CHECK(flags_before == 1);
    table_of(w).death_date[3] = year(1711);
    table_of(w).cause_of_death[3] = 2;
    table_of(w).health[3] = Fixed32::zero();
    table_of(w).planned_death[3] = kNoDate;
    set_slot_state(w, CharacterId(3), 2);
    const auto problems = w.check_invariants();
    CHECK(mentions(problems, "life-event queue holds an entry"));
    CHECK(mentions(problems, "dead character 3"));  // still in the member list of its house
}

TEST_CASE("save: random damage to a world with an archive is handled safely") {
    World w = world_with_archive();
    Pcg32 rng(404, 7);
    for (int i = 0; i < 40; ++i) {  // more people, more deaths
        const CharacterId id =
            add(w, rng.chance_permille(500) ? Sex::Male : Sex::Female, year(1660) + i * 90);
        if (i % 3 == 0) {
            REQUIRE(w.die(id, static_cast<CauseOfDeath>(1 + i % 9)));
        }
    }
    require_valid(w);
    const std::vector<std::uint8_t> bytes = state_bytes(w);
    REQUIRE(load(bytes).has_value());

    std::size_t survived = 0;
    for (int round = 0; round < 800; ++round) {
        std::vector<std::uint8_t> damaged = bytes;
        const int flips = 1 + static_cast<int>(rng.bounded(4));
        for (int k = 0; k < flips; ++k) {
            const std::size_t where = rng.bounded(static_cast<std::uint32_t>(damaged.size()));
            damaged[where] = static_cast<std::uint8_t>(rng.bounded(256));
        }
        if (round % 7 == 0) {
            damaged.resize(damaged.size() - 1 - rng.bounded(40));
        }
        const auto result = load(damaged);
        if (result.has_value()) {  // an accepted file is a valid world
            ++survived;
            CHECK(result.value().check_invariants().empty());
        }
    }
    CHECK(survived < 800);

    // every prefix of the payload is an error
    for (std::size_t n = 0; n < bytes.size(); n += 3) {
        CHECK_FALSE(load(std::span<const std::uint8_t>(bytes.data(), n)).has_value());
    }

    // every slot byte set to every other state: accepted only as a valid world
    std::vector<std::uint8_t> marker;
    {
        ByteWriter m;
        m.u32(static_cast<std::uint32_t>(w.characters().ids.capacity()));
        m.bytes(w.characters().ids.alive_flags());
        marker = m.take();
    }
    const auto at = std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end());
    REQUIRE(at != bytes.end());
    const std::size_t table = static_cast<std::size_t>(at - bytes.begin()) + 4;
    for (std::size_t i = 0; i < w.characters().ids.capacity(); ++i) {
        for (const std::uint8_t flag :
             {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{3}, std::uint8_t{255}}) {
            std::vector<std::uint8_t> damaged = bytes;
            damaged[table + i] = flag;
            const auto result = load(damaged);
            if (result.has_value()) {
                CHECK(result.value().check_invariants().empty());
            }
        }
    }
}

// =================================================================================================
// a simulation in which people die: journal, save in the middle, replay
// =================================================================================================

namespace {

/// Creates people, marries some and lets others die, all through commands (internal ones, regenerated
/// by the system when a replay is played).
class Churn final : public System {
public:
    [[nodiscard]] std::string_view name() const override { return "test.churn"; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }
    void run(World& world, TickContext& context) override {
        const auto tick = static_cast<std::size_t>(context.tick_index);
        const auto living = world.living();
        const CharacterTable& t = world.characters();
        if (tick % 6 == 0) {
            cmd::CreatePerson person;
            person.init.dynasty = DynastyId(static_cast<std::uint32_t>(tick / 6) % 3);
            person.init.birth_date = context.date - 20 * 365 - static_cast<int>(tick % 400);
            person.init.sex = tick % 12 == 0 ? Sex::Male : Sex::Female;
            person.init.planned_death = context.date + 4000;
            context.emit(Command{DynastyId(), person});
        }
        if (tick % 11 == 0 && living.size() >= 2) {
            CharacterId man;
            CharacterId woman;
            for (const CharacterId c : living) {
                if (t.spouse[c.index()].valid() || t.marriages[c.index()] >= 2) {
                    continue;
                }
                if (t.sex[c.index()] == Sex::Male && !man.valid()) {
                    man = c;
                } else if (t.sex[c.index()] == Sex::Female && !woman.valid()) {
                    woman = c;
                }
            }
            if (man.valid() && woman.valid()) {
                context.emit(Command{DynastyId(), cmd::Marry{man, woman}});
            }
        }
        if (tick % 3 == 0 && living.size() > 8) {
            const CharacterId victim = living[(tick * 7) % living.size()];
            context.emit(Command{DynastyId(), cmd::Die{victim, static_cast<CauseOfDeath>(1 + tick % 9)}});
        }
        if (tick % 50 == 49) {  // a second death of somebody who is dead already: refused, journalled
            for (std::uint32_t i = 0; i < t.ids.capacity(); ++i) {
                if (world.is_archived(CharacterId(i))) {
                    context.emit(Command{DynastyId(), cmd::Die{CharacterId(i), CauseOfDeath::Murder}});
                    break;
                }
            }
        }
    }
};

Simulation churn_simulation() {
    World world = dynasty_world(21, 3);
    for (int i = 0; i < 12; ++i) {
        (void)add(world,
                  i % 2 == 0 ? Sex::Male : Sex::Female,
                  year(1660 + i),
                  CharacterId(),
                  CharacterId(),
                  DynastyId(static_cast<std::uint32_t>(i % 3)));
    }
    Simulation sim(std::move(world));
    sim.add_system(std::make_unique<Churn>());
    return sim;
}

}  // namespace

TEST_CASE("simulation: deaths reach the journal of the tick, repeated ones are refused") {
    Simulation sim = churn_simulation();
    sim.run_days(200);
    require_valid(sim.world());
    CHECK(sim.world().archived_count() > 20);
    CHECK(sim.world().compute_hash().ext[ext_index(ExtPart::Archive)] != 0);
    std::size_t accepted = 0;
    std::size_t refused = 0;
    for (const LoggedCommand& entry : sim.command_log().entries()) {
        if (const auto* die = std::get_if<cmd::Die>(&entry.command.payload)) {
            if (entry.accepted()) {
                ++accepted;
                CHECK(sim.world().is_archived(die->character));
                CHECK(sim.world().characters().death_date[die->character.index()] <= entry.date);
            } else {
                ++refused;
                CHECK(entry.outcome == Rejection::InvalidState);
            }
        }
    }
    CHECK(accepted == sim.world().archived_count());
    CHECK(refused >= 3);
}

TEST_CASE("simulation: a save with an archive and a pending Die resumes identically") {
    Simulation original = churn_simulation();
    original.run_days(150);
    // find a moment when a Die is pending, so the save carries it
    for (int i = 0;
         i < 6 && !std::any_of(
                      original.pending_internal().begin(),
                      original.pending_internal().end(),
                      [](const Command& c) { return std::holds_alternative<cmd::Die>(c.payload); });
         ++i) {
        original.run_days(1);
    }
    REQUIRE(std::any_of(original.pending_internal().begin(),
                        original.pending_internal().end(),
                        [](const Command& c) { return std::holds_alternative<cmd::Die>(c.payload); }));
    REQUIRE(original.world().archived_count() > 10);

    const auto bytes = serialize_save(original.world(), original.pending_internal());
    Simulation resumed = churn_simulation();
    auto loaded = deserialize_save(bytes);
    REQUIRE(loaded.has_value());
    resumed.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
    CHECK(resumed.world().compute_hash() == original.world().compute_hash());

    original.run_days(400);
    resumed.run_days(400);
    CHECK(resumed.world().compute_hash() == original.world().compute_hash());
    CHECK(resumed.world().characters() == original.world().characters());
    CHECK(resumed.world().archived_count() == original.world().archived_count());
    require_valid(original.world());
    require_valid(resumed.world());
}

TEST_CASE("simulation: a replay with Die reproduces the hash, archive included") {
    Simulation recorded = churn_simulation();
    ReplayRecorder recorder(21, 0xD1E5ULL, recorded.world().today(), 30);
    CharacterId victim;
    for (int day = 0; day < 260; ++day) {
        if (day == 40) {  // an external death, recorded as input of the replay
            REQUIRE_FALSE(recorded.world().living().empty());
            victim = recorded.world().living().back();
            REQUIRE(recorded.submit(Command{DynastyId(), cmd::Die{victim, CauseOfDeath::Execution}}));
        }
        (void)recorded.tick();
        recorder.after_tick(recorded);
    }
    const Replay replay = recorder.finish(recorded);
    REQUIRE(std::any_of(replay.commands.begin(), replay.commands.end(), [](const ReplayCommand& c) {
        return std::holds_alternative<cmd::Die>(c.command.payload);
    }));
    // only the external Die is stored; the deaths of the system are produced again
    std::size_t stored = 0;
    for (const ReplayCommand& c : replay.commands) {
        stored += std::holds_alternative<cmd::Die>(c.command.payload) ? 1 : 0;
    }
    CHECK(stored == 1);
    CHECK(recorded.world().is_archived(victim));
    CHECK(recorded.world().archived_person(victim)->cause == CauseOfDeath::Execution);
    CHECK(recorded.world().archived_count() > 30);

    auto parsed = deserialize_replay(serialize_replay(replay));
    REQUIRE(parsed.has_value());
    Simulation again = churn_simulation();
    const ReplayResult result = play_replay(again, parsed.value());
    CHECK(result.ok);
    CHECK(again.world().compute_hash() == recorded.world().compute_hash());
    CHECK(again.world().characters() == recorded.world().characters());
    CHECK(again.world().compute_hash().ext[ext_index(ExtPart::Archive)] ==
          recorded.world().compute_hash().ext[ext_index(ExtPart::Archive)]);
}

// =================================================================================================
// a property test against an independent model
// =================================================================================================

namespace {

struct ModelPerson {
    std::uint32_t dynasty = 0;
    Sex sex = Sex::Male;
    int birth = 0;
    int father = -1;
    int mother = -1;
    bool alive = true;
    int spouse = -1;
    int marriages = 0;
    std::vector<int> ex;
    int death = 0;
    int cause = 0;
};

struct Model {
    std::vector<ModelPerson> people;
    std::array<int, 3> head{-1, -1, -1};
    int today = 0;

    [[nodiscard]] bool valid_parent(int parent, Sex role, int child_birth) const {
        return parent >= 0 && parent < static_cast<int>(people.size()) &&
               people[static_cast<std::size_t>(parent)].sex == role &&
               people[static_cast<std::size_t>(parent)].birth < child_birth;
    }
    [[nodiscard]] bool can_marry(int a, int b) const {
        if (a == b) {
            return false;
        }
        const ModelPerson& pa = people[static_cast<std::size_t>(a)];
        const ModelPerson& pb = people[static_cast<std::size_t>(b)];
        return pa.alive && pb.alive && pa.spouse < 0 && pb.spouse < 0 && pa.marriages < 3 &&
               pb.marriages < 3 && std::find(pa.ex.begin(), pa.ex.end(), b) == pa.ex.end();
    }
    void marry(int a, int b) {
        people[static_cast<std::size_t>(a)].spouse = b;
        people[static_cast<std::size_t>(b)].spouse = a;
        ++people[static_cast<std::size_t>(a)].marriages;
        ++people[static_cast<std::size_t>(b)].marriages;
    }
    void end_marriage(int a) {
        const int b = people[static_cast<std::size_t>(a)].spouse;
        people[static_cast<std::size_t>(a)].ex.push_back(b);
        people[static_cast<std::size_t>(b)].ex.push_back(a);
        people[static_cast<std::size_t>(a)].spouse = -1;
        people[static_cast<std::size_t>(b)].spouse = -1;
    }
    void die(int a, int cause) {
        ModelPerson& p = people[static_cast<std::size_t>(a)];
        if (p.spouse >= 0) {
            end_marriage(a);
        }
        p.alive = false;
        p.death = today;
        p.cause = cause;
        for (int& h : head) {
            h = h == a ? -1 : h;
        }
    }
    [[nodiscard]] std::vector<int> children_of(int parent) const {
        std::vector<int> out;
        for (std::size_t i = 0; i < people.size(); ++i) {
            if (people[i].father == parent || people[i].mother == parent) {
                out.push_back(static_cast<int>(i));
            }
        }
        std::sort(out.begin(), out.end(), [&](int a, int b) {
            const ModelPerson& pa = people[static_cast<std::size_t>(a)];
            const ModelPerson& pb = people[static_cast<std::size_t>(b)];
            return pa.birth != pb.birth ? pa.birth < pb.birth : a < b;
        });
        return out;
    }
};

int raw_id(CharacterId id) {
    return id.valid() ? static_cast<int>(id.value()) : -1;
}

/// The first difference between the world and the model, or "".
std::string difference(const World& w, const Model& m) {
    const CharacterTable& t = w.characters();
    if (t.ids.capacity() != m.people.size()) {
        return "capacity";
    }
    std::vector<int> living;
    std::array<std::vector<int>, 3> members;
    std::size_t dead = 0;
    for (std::size_t i = 0; i < m.people.size(); ++i) {
        const ModelPerson& p = m.people[i];
        const CharacterId id(static_cast<std::uint32_t>(i));
        const std::string who = "person " + std::to_string(i) + ": ";
        if (w.is_alive(id) != p.alive || w.is_archived(id) == p.alive || !w.exists(id)) {
            return who + "state";
        }
        if (t.dynasty[i].value() != p.dynasty || t.sex[i] != p.sex ||
            t.birth_date[i].days() != year(1703).days() + p.birth) {
            return who + "identity";
        }
        if (raw_id(t.father[i]) != p.father || raw_id(t.mother[i]) != p.mother) {
            return who + "parents";
        }
        if (raw_id(t.spouse[i]) != p.spouse || t.marriages[i] != p.marriages) {
            return who + "marriage";
        }
        for (std::size_t k = 0; k < kMaxExSpouses; ++k) {
            const int expected = k < p.ex.size() ? p.ex[k] : -1;
            if (raw_id(t.ex_spouses[i][k]) != expected) {
                return who + "former spouses";
            }
        }
        std::vector<int> children;
        for (const CharacterId c : kin::children_of(w, id)) {
            children.push_back(static_cast<int>(c.value()));
        }
        if (children != m.children_of(static_cast<int>(i))) {
            return who + "children";
        }
        if (p.alive) {
            living.push_back(static_cast<int>(i));
            members[p.dynasty].push_back(static_cast<int>(i));
            if (t.death_date[i] != kNoDate || t.cause_of_death[i] != 0 || queue_entries_of(w, id) == 0) {
                return who + "living row";
            }
            if (w.archived_person(id).has_value()) {
                return who + "living in the archive";
            }
        } else {
            ++dead;
            if (t.death_date[i].days() != year(1703).days() + p.death || t.cause_of_death[i] != p.cause) {
                return who + "death";
            }
            if (queue_entries_of(w, id) != 0 || t.spouse[i].valid()) {
                return who + "dead row still scheduled or married";
            }
            const auto record = w.archived_person(id);
            if (!record.has_value() || record->cause != static_cast<CauseOfDeath>(p.cause) ||
                record->death_date != t.death_date[i] || record->birth_date != t.birth_date[i] ||
                raw_id(record->father) != p.father || raw_id(record->mother) != p.mother ||
                record->marriages != p.marriages || record->children.size() != children.size()) {
                return who + "archived record";
            }
        }
    }
    std::vector<int> actual_living;
    for (const CharacterId c : w.living()) {
        actual_living.push_back(static_cast<int>(c.value()));
    }
    if (actual_living != living || w.archived_count() != dead) {
        return "living list or archive count";
    }
    for (std::uint32_t d = 0; d < 3; ++d) {
        std::vector<int> actual;
        for (const CharacterId c : w.members_of(DynastyId(d))) {
            actual.push_back(static_cast<int>(c.value()));
        }
        std::sort(actual.begin(), actual.end());
        if (actual != members[d]) {
            return "members of dynasty " + std::to_string(d);
        }
        if (raw_id(w.dynasties().head[d]) != m.head[d]) {
            return "head of dynasty " + std::to_string(d);
        }
    }
    if (w.life_queue().size() < living.size()) {
        return "queue";
    }
    const auto problems = w.check_invariants();
    if (!problems.empty()) {
        return "invariant: " + problems.front();
    }
    return "";
}

void random_history(std::uint64_t seed, int operations, bool with_reloads) {
    World w = dynasty_world(seed, 3);
    Model m;
    Pcg32 rng(seed, 0xA5C41FULL);
    const int base = year(1703).days();
    for (int step = 0; step < operations; ++step) {
        const auto count = static_cast<std::uint32_t>(m.people.size());
        const std::uint32_t pick = rng.bounded(100);
        if (count < 6 || pick < 24) {  // a person, parents at random (many of them impossible)
            CharacterInit init;
            ModelPerson p;
            p.dynasty = rng.bounded(3);
            p.sex = rng.chance_permille(500) ? Sex::Male : Sex::Female;
            p.birth = rng.chance_permille(30) ? m.today + 1 + rng.range(0, 40)  // not born yet
                                              : -rng.range(0, 30000) + m.today;
            const int father =
                count > 0 && rng.chance_permille(800) ? static_cast<int>(rng.bounded(count)) : -1;
            const int mother =
                count > 0 && rng.chance_permille(800) ? static_cast<int>(rng.bounded(count)) : -1;
            p.father = m.valid_parent(father, Sex::Male, p.birth) ? father : -1;
            p.mother = m.valid_parent(mother, Sex::Female, p.birth) ? mother : -1;
            init.dynasty = DynastyId(p.dynasty);
            init.sex = p.sex;
            init.birth_date = Date::from_days(base + p.birth);
            init.father = father >= 0 ? CharacterId(static_cast<std::uint32_t>(father)) : CharacterId();
            init.mother = mother >= 0 ? CharacterId(static_cast<std::uint32_t>(mother)) : CharacterId();
            init.planned_death = rng.chance_permille(400) ? init.birth_date + 20000 : kNoDate;
            const CharacterId id = w.create_character(init);
            REQUIRE(id.valid());
            REQUIRE(id.value() == count);  // ids only grow
            m.people.push_back(p);
        } else if (pick < 52) {  // a marriage, wanted or not
            const int a = static_cast<int>(rng.bounded(count));
            const int b = static_cast<int>(rng.bounded(count));
            const bool expected = m.can_marry(a, b);
            const bool actual = w.marry(CharacterId(static_cast<std::uint32_t>(a)),
                                        CharacterId(static_cast<std::uint32_t>(b)));
            REQUIRE(actual == expected);
            if (expected) {
                m.marry(a, b);
            }
        } else if (pick < 60) {  // a divorce
            const int a = static_cast<int>(rng.bounded(count));
            const bool expected = m.people[static_cast<std::size_t>(a)].alive &&
                                  m.people[static_cast<std::size_t>(a)].spouse >= 0;
            const bool actual = w.end_marriage(CharacterId(static_cast<std::uint32_t>(a)));
            REQUIRE(actual == expected);
            if (expected) {
                m.end_marriage(a);
            }
        } else if (pick < 86) {  // a death, through the command, a quarter of them for somebody dead already
            const int a = static_cast<int>(rng.bounded(count));
            const int cause = rng.chance_permille(50) ? 0 : 1 + static_cast<int>(rng.bounded(9));
            const ModelPerson& p = m.people[static_cast<std::size_t>(a)];
            const bool expected = p.alive && p.birth <= m.today && cause != 0;
            const Command command{
                DynastyId(),
                cmd::Die{CharacterId(static_cast<std::uint32_t>(a)), static_cast<CauseOfDeath>(cause)}};
            const auto verdict = validate(w, command);
            REQUIRE(verdict.has_value() == expected);
            if (expected) {
                apply(w, command);
                m.die(a, cause);
            } else if (!p.alive) {
                REQUIRE(verdict.error().reason == Rejection::InvalidState);
            }
        } else if (pick < 92) {  // a head of a dynasty
            const std::uint32_t d = rng.bounded(3);
            const int a = static_cast<int>(rng.bounded(count));
            const ModelPerson& p = m.people[static_cast<std::size_t>(a)];
            const bool expected = p.alive && p.dynasty == d;
            REQUIRE(w.set_dynasty_head(DynastyId(d), CharacterId(static_cast<std::uint32_t>(a))) == expected);
            if (expected) {
                m.head[d] = a;
            }
        } else {  // time passes
            const int days = 1 + static_cast<int>(rng.bounded(20));
            for (int k = 0; k < days; ++k) {
                (void)w.advance_day();
            }
            m.today += days;
        }
        if (step % 40 == 39) {
            const std::string diff = difference(w, m);
            REQUIRE_MESSAGE(diff.empty(), "seed " << seed << " step " << step << ": " << diff);
            if (with_reloads) {  // save and load somewhere in the middle: same state, same future
                const std::vector<std::uint8_t> bytes = state_bytes(w);
                auto loaded = load(bytes);
                REQUIRE(loaded.has_value());
                REQUIRE(loaded.value().compute_hash() == w.compute_hash());
                REQUIRE(state_bytes(loaded.value()) == bytes);
                w = std::move(loaded.value());
            }
        }
    }
    const std::string diff = difference(w, m);
    REQUIRE_MESSAGE(diff.empty(), "seed " << seed << " at the end: " << diff);
    CHECK(m.people.size() > 40);
}

}  // namespace

TEST_CASE("property: random births, marriages and deaths agree with an independent model") {
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        CAPTURE(seed);
        random_history(seed, 400, false);
    }
}

TEST_CASE("property: saving and loading in the middle of a random history changes nothing") {
    for (std::uint64_t seed = 100; seed < 106; ++seed) {
        CAPTURE(seed);
        random_history(seed, 400, true);
    }
}

TEST_CASE("property: the same history gives the same archive, another history another one") {
    const auto hash_of = [](std::uint64_t seed) {
        World w = dynasty_world(seed, 3);
        Pcg32 rng(seed, 3);
        for (int i = 0; i < 60; ++i) {
            const CharacterId id =
                add(w, rng.chance_permille(500) ? Sex::Male : Sex::Female, year(1650) + i * 100);
            if (rng.chance_permille(450)) {
                REQUIRE(w.die(id, static_cast<CauseOfDeath>(1 + rng.bounded(9))));
            }
        }
        return w.compute_hash();
    };
    CHECK(hash_of(5) == hash_of(5));
    CHECK(hash_of(5).ext[ext_index(ExtPart::Archive)] != hash_of(6).ext[ext_index(ExtPart::Archive)]);
}
