// State schema 2 (E7-T02, ADR-0006): the character and dynasty columns, dynasty mode, the life-event
// queue, the extended hash parts, the extension block of the save, the migration from schema 1 and
// the command tags.

#include "pat/core/assert.hpp"
#include "pat/core/bytes.hpp"
#include "pat/core/compress.hpp"
#include "pat/core/hash_stream.hpp"
#include "pat/core/log.hpp"
#include "pat/core/rng.hpp"
#include "pat/sim/command.hpp"
#include "pat/sim/command_log.hpp"
#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/replay.hpp"
#include "pat/sim/save.hpp"
#include "pat/sim/system.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <vector>

using namespace pat::sim;
using pat::core::ByteReader;
using pat::core::ByteWriter;
using pat::core::Fixed32;
using pat::core::Pcg32;
using pat::core::Result;
using pat::core::StringId;

namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFU;  // an invalid id as it appears in a file
constexpr std::int32_t kNoDateRaw = std::numeric_limits<std::int32_t>::min();

Fixed32 fx(int value) {
    return Fixed32::from_int(value);
}

constexpr std::size_t ext_index(ExtPart part) {
    return static_cast<std::size_t>(part);
}

CharacterInit adult(DynastyId dynasty, Sex sex = Sex::Male) {
    CharacterInit init;
    init.dynasty = dynasty;
    init.birth_date = Date::from_ymd(1670, 5, 1);
    init.sex = sex;
    return init;
}

void require_valid(const World& world) {
    const auto problems = world.check_invariants();
    for (const auto& p : problems) {
        MESSAGE(p);
    }
    REQUIRE(problems.empty());
}

bool all_ext_parts_empty(const WorldHash& hash) {
    return std::all_of(hash.ext.begin(), hash.ext.end(), [](std::uint64_t part) { return part == 0; });
}

void ignore_assert(const pat::core::AssertInfo&) {}

/// Sorted (due, kind, character) triples of the life-event queue, without the insertion sequence.
std::vector<std::tuple<std::int32_t, int, std::uint32_t>> queue_signature(const World& world) {
    std::vector<std::tuple<std::int32_t, int, std::uint32_t>> out;
    for (const auto& entry : world.life_queue().snapshot()) {
        out.emplace_back(entry.due.days(), static_cast<int>(entry.item.kind), entry.item.character.value());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---- worlds used by several tests -------------------------------------------------------------------

/// The E1-visible content: dynasties, provinces, 30 characters. Same bytes in plain and rich worlds.
void fill_e1(World& world) {
    for (std::uint32_t i = 0; i < 3; ++i) {
        (void)world.create_dynasty(StringId(i));
    }
    for (std::uint32_t p = 0; p < 4; ++p) {
        (void)world.create_province(StringId(10 + p), DynastyId(p % 3), fx(20 + static_cast<int>(p)));
    }
    Pcg32 rng(11, 3);
    for (int i = 0; i < 30; ++i) {
        CharacterInit init;
        init.dynasty = DynastyId(static_cast<std::uint32_t>(i % 3));
        init.birth_date = Date::from_ymd(1650, 1, 1) + rng.range(0, 14000);
        init.sex = i % 2 == 0 ? Sex::Male : Sex::Female;
        init.health = fx(50 + i);
        (void)world.create_character(init);
    }
}

/// Fills the columns of E7-T02 with values that differ from the defaults.
void fill_ext(World& world) {
    std::vector<BranchId> branches;
    for (std::uint32_t d = 0; d < 3; ++d) {
        branches.push_back(world.create_branch(DynastyId(d), StringId(900 + d)));
    }
    world.set_dynasty_unity(DynastyId(0), fx(70));
    world.set_dynasty_succession_law(DynastyId(1), SuccessionLaw::Partition);
    for (std::uint32_t i = 0; i < 30; i += 2) {
        const CharacterId id(i);
        const int n = static_cast<int>(i);
        world.set_character_name(id, StringId(500 + i));
        REQUIRE(world.set_character_branch(id, branches[i % 3]));
        world.set_character_portrait(id, static_cast<std::uint16_t>(i + 1));
        world.set_character_legitimacy(id, i % 7 == 0 ? Legitimacy::Illegitimate : Legitimacy::Legitimate);
        world.set_character_skill(id, static_cast<Skill>(i % kSkillCount), fx(n % 21));
        world.set_character_fertility(id, fx(n * 3));
        world.set_character_stress(id, fx(n));
        world.set_character_piety(id, fx(90 - n));
        world.set_character_ambition(id, fx(-n));
        world.set_character_loyalty_head(id, fx(n - 10));
        world.set_character_loyalty_emperor(id, fx(10 - n));
        const std::vector<std::uint16_t> traits = {static_cast<std::uint16_t>(100 + i),
                                                   static_cast<std::uint16_t>(i + 1)};
        REQUIRE(world.set_character_traits(id, traits));
        REQUIRE(world.set_character_education(
            id,
            Education{static_cast<std::uint8_t>(1 + i % 5), CharacterId(1), static_cast<std::uint8_t>(i)}));
        REQUIRE(world.set_character_planned_death(id, world.characters().birth_date[i] + 30000 + n));
    }
}

World plain_world() {
    World world(Date::from_ymd(1703, 1, 1), 5);
    fill_e1(world);
    return world;
}

World rich_world() {
    World world(Date::from_ymd(1703, 1, 1), 5);
    REQUIRE(world.enable_dynasty_mode());
    fill_e1(world);
    fill_ext(world);
    for (int i = 0; i < 40; ++i) {
        (void)world.advance_day();
    }
    return world;
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

// ---- hand-made payloads ---------------------------------------------------------------------------------

struct RawSection {
    std::uint16_t id = 0;
    std::uint16_t version = 1;
    std::vector<std::uint8_t> body;
};

/// Replaces the empty extension block at the end of the state of an extension-free world.
std::vector<std::uint8_t> with_extension(const std::vector<std::uint8_t>& plain_state,
                                         std::uint32_t flags,
                                         const std::vector<RawSection>& sections,
                                         int declared_count = -1) {
    REQUIRE(plain_state.size() > 6);
    REQUIRE(std::all_of(plain_state.end() - 6, plain_state.end(), [](std::uint8_t b) { return b == 0; }));
    ByteWriter out;
    out.bytes(std::span<const std::uint8_t>(plain_state.data(), plain_state.size() - 6));
    out.u32(flags);
    out.u16(
        static_cast<std::uint16_t>(declared_count < 0 ? static_cast<int>(sections.size()) : declared_count));
    for (const RawSection& s : sections) {
        out.u16(s.id);
        out.u16(s.version);
        out.u32(static_cast<std::uint32_t>(s.body.size()));
        out.bytes(s.body);
    }
    return out.take();
}

struct PersonRow {
    std::uint32_t id = 0;
    std::uint32_t name = kNone;
    std::uint32_t branch = kNone;
    std::uint16_t portrait = 0;
    std::uint8_t legitimacy = 0;
    std::uint8_t cause = 0;
    std::int32_t death = kNoDateRaw;
    std::int32_t skills[5] = {0, 0, 0, 0, 0};
    std::int32_t scales[6] = {0, 0, 0, 0, 0, 0};  // fertility, stress, piety, ambition, loyalties
    std::uint16_t traits[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::uint8_t focus = 0;
    std::uint32_t tutor = kNone;
    std::uint8_t level = 0;
    std::int32_t planned = kNoDateRaw;
};

void write_row(ByteWriter& w, const PersonRow& r) {
    w.u32(r.id);
    w.u32(r.name);
    w.u32(r.branch);
    w.u16(r.portrait);
    w.u8(r.legitimacy);
    w.u8(r.cause);
    w.i32(r.death);
    for (const std::int32_t v : r.skills) {
        w.i32(v);
    }
    for (const std::int32_t v : r.scales) {
        w.i32(v);
    }
    for (const std::uint16_t v : r.traits) {
        w.u16(v);
    }
    w.u8(r.focus);
    w.u32(r.tutor);
    w.u8(r.level);
    w.i32(r.planned);
}

RawSection person_section(const std::vector<PersonRow>& rows) {
    ByteWriter w;
    w.u32(static_cast<std::uint32_t>(rows.size()));
    for (const PersonRow& r : rows) {
        write_row(w, r);
    }
    return RawSection{1, 1, w.take()};
}

std::int32_t raw(int value) {
    return fx(value).raw();
}

}  // namespace

// =================================================================================================
// date helpers
// =================================================================================================

TEST_CASE("anniversaries: ages and the next birthday agree, 29 February moves to 28 February") {
    const Date born = Date::from_ymd(1670, 5, 1);
    CHECK(age_years(born, born) == 0);
    CHECK(age_years(born, born - 10) == 0);
    CHECK(age_years(born, Date::from_ymd(1671, 4, 30)) == 0);
    CHECK(age_years(born, Date::from_ymd(1671, 5, 1)) == 1);
    CHECK(age_years(born, Date::from_ymd(1703, 4, 30)) == 32);
    CHECK(age_years(born, Date::from_ymd(1703, 5, 1)) == 33);
    CHECK(next_anniversary_after(born, Date::from_ymd(1703, 1, 1)) == Date::from_ymd(1703, 5, 1));
    CHECK(next_anniversary_after(born, Date::from_ymd(1703, 5, 1)) == Date::from_ymd(1704, 5, 1));
    CHECK(next_anniversary_on_or_after(born, Date::from_ymd(1703, 5, 1)) == Date::from_ymd(1703, 5, 1));
    CHECK(next_anniversary_on_or_after(born, Date::from_ymd(1703, 5, 2)) == Date::from_ymd(1704, 5, 1));
    // The first anniversary is one year after the birth, never the birth date itself.
    CHECK(next_anniversary_after(born, born - 5) == Date::from_ymd(1671, 5, 1));
    CHECK(next_anniversary_on_or_after(born, born) == Date::from_ymd(1671, 5, 1));

    // Born on 29 February 1704 (a leap year): 28 February in common years, 29 in leap years.
    const Date leap = Date::from_ymd(1704, 2, 29);
    CHECK(anniversary_in_year(leap, 1705) == Date::from_ymd(1705, 2, 28));
    CHECK(anniversary_in_year(leap, 1706) == Date::from_ymd(1706, 2, 28));
    CHECK(anniversary_in_year(leap, 1708) == Date::from_ymd(1708, 2, 29));
    CHECK(age_years(leap, Date::from_ymd(1705, 2, 27)) == 0);
    CHECK(age_years(leap, Date::from_ymd(1705, 2, 28)) == 1);
    CHECK(age_years(leap, Date::from_ymd(1708, 2, 28)) == 3);
    CHECK(age_years(leap, Date::from_ymd(1708, 2, 29)) == 4);
    // One processing per year, on the day age_years() increases.
    Date cursor = leap;
    for (int year = 1705; year <= 1716; ++year) {
        cursor = next_anniversary_after(leap, cursor);
        CHECK(cursor == anniversary_in_year(leap, year));
        CHECK(age_years(leap, cursor) == year - 1704);
        CHECK(age_years(leap, cursor - 1) == year - 1705);
    }
    // Born 28 February: the same date every year, also in leap years.
    const Date february = Date::from_ymd(1703, 2, 28);
    CHECK(anniversary_in_year(february, 1704) == Date::from_ymd(1704, 2, 28));
}

// =================================================================================================
// entity table: retired slots
// =================================================================================================

TEST_CASE("entity table: a retired slot is never handed out again") {
    EntityTable<CharacterId> table;
    const CharacterId a = table.create();
    const CharacterId b = table.create();
    const CharacterId c = table.create();
    CHECK(table.retire(b));
    CHECK_FALSE(table.retire(b));  // already retired
    CHECK_FALSE(table.is_alive(b));
    CHECK(table.alive_count() == 2);
    CHECK(table.retired_count() == 1);
    CHECK(table.free_list().empty());
    CHECK(table.create().value() == 3);  // not 1
    CHECK(table.destroy(c));             // an ordinary slot still frees normally
    CHECK(table.create() == c);
    CHECK_FALSE(table.destroy(b));  // destroy only works on live slots
    CHECK(table.alive_flags()[a.index()] == 1);
    CHECK(table.alive_flags()[b.index()] == 2);

    // restore() accepts the third state and counts it.
    EntityTable<CharacterId> copy;
    REQUIRE(copy.restore({1, 2, 0, 1}, {0, 0, 1, 0}, {2}));
    CHECK(copy.alive_count() == 2);
    CHECK(copy.retired_count() == 1);
    CHECK(copy.create().value() == 2);  // the only free slot
    CHECK(copy.create().value() == 4);
    EntityTable<CharacterId> bad;
    CHECK_FALSE(bad.restore({1, 3}, {0, 0}, {}));   // unknown state
    CHECK_FALSE(bad.restore({2, 0}, {0, 0}, {0}));  // a retired slot in the free list
    CHECK_FALSE(bad.restore({2, 1}, {0, 0}, {0}));  // free list entry that is not free
}

// =================================================================================================
// columns and the World API
// =================================================================================================

TEST_CASE("columns: a new character starts at the defaults and costs nothing in the hash") {
    World world(Date::from_ymd(1703, 1, 1), 1);
    const DynastyId d = world.create_dynasty(StringId(0));
    const CharacterId c = world.create_character(adult(d));
    const CharacterTable& t = world.characters();
    const std::size_t i = c.index();

    CHECK_FALSE(t.name[i].valid());
    CHECK_FALSE(t.branch[i].valid());
    CHECK(t.portrait[i] == 0);
    CHECK(t.legitimacy[i] == Legitimacy::Legitimate);
    CHECK(t.cause_of_death[i] == 0);
    CHECK(t.death_date[i] == kNoDate);
    for (const Fixed32 skill : t.skills[i]) {
        CHECK(skill == Fixed32());
    }
    CHECK(t.fertility[i] == Fixed32());
    CHECK(t.stress[i] == Fixed32());
    CHECK(t.piety[i] == Fixed32());
    CHECK(t.ambition[i] == Fixed32());
    CHECK(t.loyalty_head[i] == Fixed32());
    CHECK(t.loyalty_emperor[i] == Fixed32());
    for (const std::uint16_t trait : t.traits[i]) {
        CHECK(trait == 0);
    }
    CHECK(t.education_focus[i] == 0);
    CHECK_FALSE(t.education_tutor[i].valid());
    CHECK(t.education_level[i] == 0);
    CHECK(t.planned_death[i] == kNoDate);
    CHECK(world.dynasties().unity[d.index()] == kDefaultUnity);
    CHECK(world.dynasties().succession_law[d.index()] == SuccessionLaw::Majorat);
    CHECK(world.branches().size() == 0);
    CHECK_FALSE(world.dynasty_mode());
    CHECK(world.life_queue().empty());
    require_valid(world);

    const WorldHash hash = world.compute_hash();
    CHECK(all_ext_parts_empty(hash));
    // With nothing extended, `total` is exactly the E1 formula over the five parts.
    pat::core::HashStream legacy;
    legacy.u64(hash.calendar);
    legacy.u64(hash.rng);
    legacy.u64(hash.dynasties);
    legacy.u64(hash.provinces);
    legacy.u64(hash.characters);
    CHECK(hash.total == legacy.value());
}

TEST_CASE("columns: create_character stores the init and clamps what would break an invariant") {
    World world(Date::from_ymd(1703, 1, 1), 1);
    const DynastyId d = world.create_dynasty(StringId(0));
    const DynastyId other = world.create_dynasty(StringId(1));
    const BranchId mine = world.create_branch(d, StringId(70));
    const BranchId foreign = world.create_branch(other, StringId(71));
    const CharacterId tutor = world.create_character(adult(d));

    CharacterInit init = adult(d, Sex::Female);
    init.name = StringId(123);
    init.branch = mine;
    init.portrait = 9;
    init.legitimacy = Legitimacy::Legitimized;
    init.skills = {fx(1), fx(2), fx(3), fx(25), fx(-4)};  // the last two are out of range
    init.fertility = fx(150);
    init.stress = fx(-1);
    init.piety = fx(55);
    init.ambition = fx(-300);
    init.loyalty_head = fx(80);
    init.loyalty_emperor = fx(300);
    init.traits = {40, 0, 7, 7, 300, 0, 2, 0};  // unsorted, a repeat, gaps
    init.education = Education{3, tutor, 250};
    init.planned_death = init.birth_date + 20000;
    const CharacterId c = world.create_character(init);
    const CharacterTable& t = world.characters();
    const std::size_t i = c.index();
    CHECK(t.name[i] == StringId(123));
    CHECK(t.branch[i] == mine);
    CHECK(t.portrait[i] == 9);
    CHECK(t.legitimacy[i] == Legitimacy::Legitimized);
    CHECK(t.skills[i][0] == fx(1));
    CHECK(t.skills[i][3] == kSkillMax);
    CHECK(t.skills[i][4] == Fixed32());
    CHECK(t.fertility[i] == kScaleMax);
    CHECK(t.stress[i] == Fixed32());
    CHECK(t.piety[i] == fx(55));
    CHECK(t.ambition[i] == fx(-100));
    CHECK(t.loyalty_head[i] == fx(80));
    CHECK(t.loyalty_emperor[i] == fx(100));
    const std::array<std::uint16_t, kTraitSlots> expected_traits = {2, 7, 40, 300, 0, 0, 0, 0};
    CHECK(t.traits[i] == expected_traits);
    CHECK(t.education_focus[i] == 3);
    CHECK(t.education_tutor[i] == tutor);
    CHECK(t.education_level[i] == kMaxEducationLevel);
    CHECK(t.planned_death[i] == init.birth_date + 20000);
    require_valid(world);

    // References that cannot be kept are dropped, not stored.
    CharacterInit odd = adult(d);
    odd.branch = foreign;                              // belongs to another dynasty
    odd.education = Education{9, CharacterId(77), 3};  // unknown focus, unknown tutor
    odd.planned_death = odd.birth_date - 5;            // before the birth
    const CharacterId o = world.create_character(odd);
    CHECK_FALSE(t.branch[o.index()].valid());
    CHECK(t.education_focus[o.index()] == 0);
    CHECK_FALSE(t.education_tutor[o.index()].valid());
    CHECK(t.education_level[o.index()] == 3);
    CHECK(t.planned_death[o.index()] == kNoDate);
    require_valid(world);
}

TEST_CASE("columns: setters clamp, validate and ignore the dead") {
    World world(Date::from_ymd(1703, 1, 1), 1);
    const DynastyId d = world.create_dynasty(StringId(0));
    const DynastyId other = world.create_dynasty(StringId(1));
    const CharacterId a = world.create_character(adult(d));
    const CharacterId b = world.create_character(adult(d));
    const BranchId branch = world.create_branch(d, StringId(70));
    const BranchId foreign = world.create_branch(other, StringId(71));
    const CharacterTable& t = world.characters();

    world.set_character_skill(a, Skill::Intrigue, fx(99));
    world.set_character_skill(a, Skill::Learning, fx(-5));
    CHECK(t.skills[a.index()][static_cast<std::size_t>(Skill::Intrigue)] == kSkillMax);
    CHECK(t.skills[a.index()][static_cast<std::size_t>(Skill::Learning)] == Fixed32());
    world.set_character_fertility(a, fx(120));
    world.set_character_stress(a, fx(-3));
    world.set_character_piety(a, fx(42));
    world.set_character_ambition(a, fx(-250));
    world.set_character_loyalty_head(a, fx(250));
    world.set_character_loyalty_emperor(a, fx(-7));
    CHECK(t.fertility[a.index()] == kScaleMax);
    CHECK(t.stress[a.index()] == Fixed32());
    CHECK(t.piety[a.index()] == fx(42));
    CHECK(t.ambition[a.index()] == fx(-100));
    CHECK(t.loyalty_head[a.index()] == fx(100));
    CHECK(t.loyalty_emperor[a.index()] == fx(-7));
    world.set_dynasty_unity(d, fx(500));
    CHECK(world.dynasties().unity[d.index()] == kScaleMax);
    world.set_dynasty_unity(d, fx(-5));
    CHECK(world.dynasties().unity[d.index()] == Fixed32());

    // Traits: stored packed and sorted; too many or repeated traits change nothing.
    const std::vector<std::uint16_t> good = {9, 0, 3, 5};
    CHECK(world.set_character_traits(a, good));
    const std::array<std::uint16_t, kTraitSlots> packed = {3, 5, 9, 0, 0, 0, 0, 0};
    CHECK(t.traits[a.index()] == packed);
    const std::vector<std::uint16_t> repeated = {4, 4};
    const std::vector<std::uint16_t> too_many = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK_FALSE(world.set_character_traits(a, repeated));
    CHECK_FALSE(world.set_character_traits(a, too_many));
    CHECK(t.traits[a.index()] == packed);
    const std::vector<std::uint16_t> none;
    CHECK(world.set_character_traits(a, none));
    CHECK(t.traits[a.index()] == std::array<std::uint16_t, kTraitSlots>{});

    // Education.
    CHECK(world.set_character_education(a, Education{2, b, 10}));
    CHECK_FALSE(world.set_character_education(a, Education{6, b, 10}));
    CHECK_FALSE(world.set_character_education(a, Education{2, b, 101}));
    CHECK_FALSE(world.set_character_education(a, Education{2, a, 10}));  // nobody tutors themselves
    CHECK_FALSE(world.set_character_education(a, Education{2, CharacterId(40), 10}));
    CHECK(t.education_focus[a.index()] == 2);
    CHECK(t.education_tutor[a.index()] == b);
    CHECK(t.education_level[a.index()] == 10);

    // Branches belong to one dynasty and are dropped when the character changes dynasty.
    CHECK(world.set_character_branch(a, branch));
    CHECK_FALSE(world.set_character_branch(a, foreign));
    CHECK_FALSE(world.set_character_branch(a, BranchId(99)));
    CHECK(t.branch[a.index()] == branch);
    world.set_character_dynasty(a, other);
    CHECK_FALSE(t.branch[a.index()].valid());
    CHECK(world.set_character_branch(a, foreign));
    require_valid(world);

    // Planned death must lie in [birth, birth + 200 years].
    const Date birth = t.birth_date[b.index()];
    CHECK(world.set_character_planned_death(b, birth + 30000));
    CHECK(t.planned_death[b.index()] == birth + 30000);
    CHECK_FALSE(world.set_character_planned_death(b, birth - 1));
    CHECK_FALSE(world.set_character_planned_death(b, birth + kMaxLifeSpanDays + 1));
    CHECK(t.planned_death[b.index()] == birth + 30000);
    CHECK(world.set_character_planned_death(b, kNoDate));
    CHECK(t.planned_death[b.index()] == kNoDate);

    // Dead characters are ignored by every setter.
    REQUIRE(world.destroy_character(b));
    world.set_character_name(b, StringId(5));
    world.set_character_skill(b, Skill::Martial, fx(5));
    CHECK_FALSE(world.set_character_branch(b, branch));
    CHECK_FALSE(world.set_character_planned_death(b, birth + 100));
    CHECK_FALSE(world.set_character_traits(b, good));
    require_valid(world);
}

TEST_CASE("columns: destroying a character clears every column, a reused slot starts clean") {
    World world(Date::from_ymd(1703, 1, 1), 1);
    const DynastyId d = world.create_dynasty(StringId(0));
    const BranchId branch = world.create_branch(d, StringId(70));
    const CharacterId a = world.create_character(adult(d));
    REQUIRE(world.set_character_branch(a, branch));
    world.set_character_name(a, StringId(5));
    world.set_character_skill(a, Skill::Stewardship, fx(12));
    world.set_character_stress(a, fx(30));
    REQUIRE(world.set_character_planned_death(a, Date::from_ymd(1700, 1, 1) + 40000));
    const std::vector<std::uint16_t> traits = {6};
    REQUIRE(world.set_character_traits(a, traits));
    REQUIRE(world.destroy_character(a));
    require_valid(world);  // a free slot holding data is an invariant violation
    CHECK(all_ext_parts_empty(world.compute_hash()) == false);  // branches still exist: DynastyExt

    const CharacterId again = world.create_character(adult(d));
    CHECK(again == a);
    CHECK_FALSE(world.characters().name[again.index()].valid());
    CHECK(world.characters().skills[again.index()][0] == Fixed32());
    CHECK(world.characters().planned_death[again.index()] == kNoDate);
    CHECK(world.compute_hash().ext[ext_index(ExtPart::CharacterExt)] == 0);
    CHECK(world.living().size() == 1);
}

// =================================================================================================
// extended hash parts
// =================================================================================================

TEST_CASE("hash: extended parts appear with data, stay out of the five parts and cancel when reverted") {
    World world = plain_world();
    const WorldHash base = world.compute_hash();
    REQUIRE(all_ext_parts_empty(base));

    world.set_character_skill(CharacterId(4), Skill::Martial, fx(9));
    const WorldHash with_skill = world.compute_hash();
    CHECK(with_skill.ext[ext_index(ExtPart::CharacterExt)] != 0);
    CHECK(with_skill.total != base.total);
    CHECK(with_skill.calendar == base.calendar);
    CHECK(with_skill.rng == base.rng);
    CHECK(with_skill.dynasties == base.dynasties);
    CHECK(with_skill.provinces == base.provinces);
    CHECK(with_skill.characters == base.characters);

    // The same value reached another way gives the same hash; a different value does not.
    World other = plain_world();
    other.set_character_skill(CharacterId(4), Skill::Martial, fx(3));
    other.set_character_skill(CharacterId(4), Skill::Martial, fx(9));
    CHECK(other.compute_hash() == with_skill);
    other.set_character_skill(CharacterId(4), Skill::Martial, fx(10));
    CHECK(other.compute_hash().total != with_skill.total);

    // Back to the default: the part is empty again and `total` is the old one.
    world.set_character_skill(CharacterId(4), Skill::Martial, Fixed32());
    CHECK(world.compute_hash() == base);

    // Dynasty data: a branch or a non-default unity or law is hashed, the default unity is not.
    world.set_dynasty_unity(DynastyId(1), kDefaultUnity);
    CHECK(world.compute_hash() == base);
    world.set_dynasty_unity(DynastyId(1), fx(33));
    CHECK(world.compute_hash().ext[ext_index(ExtPart::DynastyExt)] != 0);
    world.set_dynasty_unity(DynastyId(1), kDefaultUnity);
    world.set_dynasty_succession_law(DynastyId(2), SuccessionLaw::FamilyCouncil);
    CHECK(world.compute_hash().ext[ext_index(ExtPart::DynastyExt)] != 0);
    world.set_dynasty_succession_law(DynastyId(2), SuccessionLaw::Majorat);
    CHECK(world.compute_hash() == base);
    (void)world.create_branch(DynastyId(0), StringId(70));
    CHECK(world.compute_hash().ext[ext_index(ExtPart::DynastyExt)] != 0);

    // The mode is hashed, but changes none of the five parts.
    World mode = plain_world();
    REQUIRE(mode.enable_dynasty_mode());
    const WorldHash moded = mode.compute_hash();
    CHECK(moded.ext[ext_index(ExtPart::Mode)] != 0);
    CHECK(moded.total != base.total);
    CHECK(moded.characters == base.characters);
    CHECK(moded.dynasties == base.dynasties);
    CHECK(moded.same_recorded_parts(base) == false);
    CHECK(base.same_recorded_parts(base));
    WorldHash tweaked = base;
    tweaked.ext[5] = 123;  // the extended parts are not part of a checkpoint
    CHECK(tweaked.same_recorded_parts(base));
    CHECK(tweaked != base);
}

TEST_CASE("hash: total is built from the five parts first and the used extended parts in order") {
    World world = plain_world();
    world.set_character_stress(CharacterId(2), fx(4));
    (void)world.create_branch(DynastyId(0), StringId(70));
    REQUIRE(world.enable_dynasty_mode());
    const WorldHash h = world.compute_hash();

    pat::core::HashStream expected;
    expected.u64(h.calendar);
    expected.u64(h.rng);
    expected.u64(h.dynasties);
    expected.u64(h.provinces);
    expected.u64(h.characters);
    for (std::size_t part = 0; part < kExtPartCount; ++part) {
        if (h.ext[part] != 0) {
            expected.u8(static_cast<std::uint8_t>(part));
            expected.u64(h.ext[part]);
        }
    }
    CHECK(h.total == expected.value());
    CHECK(h.ext[ext_index(ExtPart::Mode)] != 0);
    CHECK(h.ext[ext_index(ExtPart::CharacterExt)] != 0);
    CHECK(h.ext[ext_index(ExtPart::DynastyExt)] != 0);
    CHECK(h.ext[ext_index(ExtPart::Archive)] == 0);
    CHECK(ext_part_name(ext_index(ExtPart::CharacterExt)) == "character_ext");
    CHECK(ext_part_name(40) == "ext[40]");
}

// =================================================================================================
// dynasty mode and the life-event queue
// =================================================================================================

TEST_CASE("dynasty mode: ids are never reused and the old destroy operation is refused") {
    pat::core::ScopedLogCapture capture;
    pat::core::ScopedAssertHandler handler{&ignore_assert};

    World world(Date::from_ymd(1703, 1, 1), 1);
    const DynastyId d = world.create_dynasty(StringId(0));
    REQUIRE(world.enable_dynasty_mode());
    CHECK(world.enable_dynasty_mode());  // idempotent
    CHECK(world.dynasty_mode());
    CHECK((world.world_flags() & kWorldFlagDynastyMode) != 0);

    const CharacterId a = world.create_character(adult(d));
    const CharacterId b = world.create_character(adult(d));
    const CharacterId c = world.create_character(adult(d));
    CHECK(a.value() == 0);
    CHECK(b.value() == 1);
    CHECK(c.value() == 2);
    CHECK_FALSE(world.destroy_character(b));  // refused: the dead would free their id
    CHECK(world.is_alive(b));
    CHECK(world.characters().ids.free_list().empty());
    CHECK(world.create_character(adult(d)).value() == 3);
    CHECK(world.living().size() == 4);
    require_valid(world);

    // The old command is refused in this mode with a hint, so it can never reach apply().
    World& w = world;
    const auto verdict = validate(w, Command{DynastyId(), cmd::KillCharacter{a}});
    REQUIRE_FALSE(verdict.has_value());
    CHECK(verdict.error().reason == Rejection::InvalidState);
}

TEST_CASE("dynasty mode: refused while freed ids are waiting, and E1 mode keeps reusing ids") {
    World world(Date::from_ymd(1703, 1, 1), 1);
    const DynastyId d = world.create_dynasty(StringId(0));
    const CharacterId a = world.create_character(adult(d));
    const CharacterId b = world.create_character(adult(d));
    REQUIRE(world.destroy_character(a));
    CHECK_FALSE(world.enable_dynasty_mode());  // id 0 would be reused
    CHECK_FALSE(world.dynasty_mode());
    CHECK(world.create_character(adult(d)) == a);  // LIFO reuse as in E1
    CHECK(world.enable_dynasty_mode());            // nothing is waiting now
    CHECK(world.is_alive(b));
    CHECK(world.life_queue().size() == 2);  // one birthday for each of the living
    require_valid(world);
}

TEST_CASE("dynasty mode: the list of the living is ascending in both modes") {
    World e1(Date::from_ymd(1703, 1, 1), 1);
    const DynastyId d = e1.create_dynasty(StringId(0));
    std::vector<CharacterId> ids;
    for (int i = 0; i < 8; ++i) {
        ids.push_back(e1.create_character(adult(d)));
    }
    REQUIRE(e1.destroy_character(ids[5]));
    REQUIRE(e1.destroy_character(ids[2]));
    (void)e1.create_character(adult(d));  // takes slot 2 again (LIFO)
    (void)e1.create_character(adult(d));  // takes slot 5
    (void)e1.create_character(adult(d));  // a fresh slot
    const auto living = e1.living();
    REQUIRE(living.size() == 9);
    CHECK(std::is_sorted(living.begin(), living.end()));
    require_valid(e1);
}

TEST_CASE("life queue: birthdays and planned deaths are derived from the columns") {
    World world(Date::from_ymd(1703, 1, 1), 1);
    const DynastyId d = world.create_dynasty(StringId(0));
    REQUIRE(world.enable_dynasty_mode());

    const CharacterId elder = world.create_character(adult(d));  // born 1670-05-01
    CharacterInit baby = adult(d);
    baby.birth_date = world.today();
    const CharacterId newborn = world.create_character(baby);
    CharacterInit leap = adult(d);
    leap.birth_date = Date::from_ymd(1700, 2, 29);
    const CharacterId leaper = world.create_character(leap);

    auto signature = queue_signature(world);
    REQUIRE(signature.size() == 3);
    const auto birthday = static_cast<int>(LifeEventKind::Birthday);
    // elder: the next anniversary; newborn: the birth date itself (the "year 0" roll); leaper: 28 February
    // 1703
    CHECK(signature[0] == std::make_tuple(world.today().days(), birthday, newborn.value()));
    CHECK(signature[1] == std::make_tuple(Date::from_ymd(1703, 2, 28).days(), birthday, leaper.value()));
    CHECK(signature[2] == std::make_tuple(Date::from_ymd(1703, 5, 1).days(), birthday, elder.value()));

    // A planned death adds an entry; changing it leaves the old one behind as a stale entry.
    const Date first = world.today() + 3000;
    const Date second = world.today() + 4000;
    REQUIRE(world.set_character_planned_death(elder, first));
    REQUIRE(world.set_character_planned_death(elder, second));
    const auto death = static_cast<int>(LifeEventKind::PlannedDeath);
    signature = queue_signature(world);
    CHECK(signature.size() == 5);
    CHECK(std::find(signature.begin(),
                    signature.end(),
                    std::make_tuple(first.days(), death, elder.value())) != signature.end());
    CHECK(std::find(signature.begin(),
                    signature.end(),
                    std::make_tuple(second.days(), death, elder.value())) != signature.end());
    require_valid(world);

    // Rebuilding keeps only what the columns say (a state at a tick boundary), strictly after today.
    for (int i = 0; i < 100; ++i) {
        (void)world.advance_day();
    }
    require_valid(world);  // overdue entries are tolerated: nothing consumes the queue yet
    world.rebuild_derived_indexes();
    signature = queue_signature(world);  // today is 1703-04-11 now
    REQUIRE(signature.size() == 4);
    CHECK(signature[0] == std::make_tuple(Date::from_ymd(1703, 5, 1).days(), birthday, elder.value()));
    CHECK(signature[1] == std::make_tuple(Date::from_ymd(1704, 1, 1).days(), birthday, newborn.value()));
    CHECK(signature[2] == std::make_tuple(Date::from_ymd(1704, 2, 29).days(), birthday, leaper.value()));
    CHECK(signature[3] == std::make_tuple(second.days(), death, elder.value()));  // the stale entry is gone
    require_valid(world);
}

TEST_CASE("life queue: a leap-day birthday gets one entry per year, on 28 February in common years") {
    World world(Date::from_ymd(1705, 1, 1), 1);
    const DynastyId d = world.create_dynasty(StringId(0));
    REQUIRE(world.enable_dynasty_mode());
    CharacterInit leap = adult(d);
    leap.birth_date = Date::from_ymd(1704, 2, 29);
    (void)world.create_character(leap);
    CHECK(queue_signature(world).front() ==
          std::make_tuple(Date::from_ymd(1705, 2, 28).days(), static_cast<int>(LifeEventKind::Birthday), 0U));
}

TEST_CASE("life queue: outside dynasty mode the queue stays empty") {
    World world = plain_world();
    CHECK(world.life_queue().empty());
    world.rebuild_derived_indexes();
    CHECK(world.life_queue().empty());
    CHECK(world.living().size() == 30);
    require_valid(world);
}

// =================================================================================================
// saving and loading
// =================================================================================================

TEST_CASE("save v2: a world without extended data gets only the empty extension block") {
    const World world = plain_world();
    const std::vector<std::uint8_t> bytes = state_bytes(world);
    ByteReader schema(std::span<const std::uint8_t>(bytes.data(), 4));
    CHECK(schema.u32() == World::kStateSchema);
    REQUIRE(bytes.size() > 6);
    CHECK(std::all_of(
        bytes.end() - 6, bytes.end(), [](std::uint8_t b) { return b == 0; }));  // flags 0, 0 sections

    auto loaded = load(bytes);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().compute_hash() == world.compute_hash());
    CHECK(state_bytes(loaded.value()) == bytes);
}

TEST_CASE("save v2: sections appear only when they have content, and each is versioned") {
    World world = plain_world();
    const std::size_t plain_size = state_bytes(world).size();
    world.set_character_stress(CharacterId(1), fx(10));
    std::vector<std::uint8_t> bytes = state_bytes(world);
    // 6-byte block header: flags (4) + count (2); then one section: id, version, length (8 bytes) + body.
    const std::size_t person_body = 4 + 90;
    CHECK(bytes.size() == plain_size + 8 + person_body);
    ByteReader in(std::span<const std::uint8_t>(bytes.data() + plain_size - 6, 6 + 8));
    CHECK(in.u32() == 0);
    CHECK(in.u16() == 1);
    CHECK(in.u16() == 1);  // section id: person
    CHECK(in.u16() == 1);  // version
    CHECK(in.u32() == person_body);

    world.set_dynasty_unity(DynastyId(0), fx(60));
    bytes = state_bytes(world);
    ByteReader two(std::span<const std::uint8_t>(bytes.data() + plain_size - 6, 6));
    CHECK(two.u32() == 0);
    CHECK(two.u16() == 2);  // person + dynasty
}

TEST_CASE("save v2: a rich dynasty-mode world round-trips and re-saves to the same bytes") {
    const World world = rich_world();
    require_valid(world);
    REQUIRE(world.compute_hash().ext[ext_index(ExtPart::CharacterExt)] != 0);

    const std::vector<std::uint8_t> bytes = state_bytes(world);
    auto loaded = load(bytes);
    REQUIRE(loaded.has_value());
    World copy = std::move(loaded).value();
    CHECK(copy.compute_hash() == world.compute_hash());
    CHECK(copy.dynasty_mode());
    CHECK(copy.characters() == world.characters());
    CHECK(copy.dynasties() == world.dynasties());
    CHECK(copy.branches() == world.branches());
    CHECK(
        std::equal(copy.living().begin(), copy.living().end(), world.living().begin(), world.living().end()));
    require_valid(copy);
    CHECK(state_bytes(copy) == bytes);

    // The derived queue is rebuilt: it equals a rebuild of the original, not necessarily the
    // original's own (nothing has consumed it, so the original still holds overdue entries).
    World rebuilt = world;
    rebuilt.rebuild_derived_indexes();
    CHECK(queue_signature(copy) == queue_signature(rebuilt));

    // The container carries it as well.
    auto container = deserialize_save(serialize_save(world));
    REQUIRE(container.has_value());
    CHECK(container.value().world.compute_hash() == world.compute_hash());
}

namespace {

/// One step of a scripted history that touches the new columns, the mode and the queue.
void script_step(World& world, int day) {
    auto& rng = world.rng().stream("script");
    if (day % 4 == 0) {
        CharacterInit init;
        init.dynasty = DynastyId(rng.bounded(3));
        init.birth_date = world.today();
        init.sex = rng.chance_permille(500) ? Sex::Male : Sex::Female;
        init.skills = {fx(rng.range(0, 20)), fx(rng.range(0, 20)), fx(1), fx(2), fx(3)};
        init.traits = {static_cast<std::uint16_t>(rng.range(1, 30)),
                       static_cast<std::uint16_t>(rng.range(31, 60))};
        init.planned_death = init.birth_date + rng.range(100, 30000);
        (void)world.create_character(init);
    }
    const auto living = world.living();
    if (!living.empty()) {
        const CharacterId who = living[rng.bounded(static_cast<std::uint32_t>(living.size()))];
        switch (rng.bounded(5)) {
            case 0:
                world.set_character_stress(who, fx(rng.range(0, 100)));
                break;
            case 1:
                world.set_character_skill(
                    who, static_cast<Skill>(rng.bounded(kSkillCount)), fx(rng.range(-2, 22)));
                break;
            case 2:
                (void)world.set_character_planned_death(
                    who, world.characters().birth_date[who.index()] + rng.range(0, 40000));
                break;
            case 3:
                world.set_character_ambition(who, fx(rng.range(-120, 120)));
                break;
            default:
                (void)world.set_character_education(
                    who, Education{static_cast<std::uint8_t>(rng.bounded(6)), CharacterId(), 5});
                break;
        }
    }
    if (day % 11 == 0) {
        world.set_dynasty_unity(DynastyId(rng.bounded(3)), fx(rng.range(0, 100)));
    }
    (void)world.advance_day();
}

}  // namespace

TEST_CASE("save v2: a world saved in the middle of a scripted history continues exactly like the original") {
    World original = rich_world();
    for (int day = 0; day < 150; ++day) {
        script_step(original, day);
    }
    require_valid(original);

    auto reloaded = deserialize_save(serialize_save(original));  // through the whole container
    REQUIRE(reloaded.has_value());
    World resumed = std::move(reloaded.value().world);
    CHECK(resumed.compute_hash() == original.compute_hash());

    for (int day = 150; day < 400; ++day) {
        script_step(original, day);
        script_step(resumed, day);
        if (day % 50 == 0) {
            REQUIRE(resumed.compute_hash() == original.compute_hash());
        }
    }
    CHECK(resumed.compute_hash() == original.compute_hash());
    CHECK(resumed.characters() == original.characters());
    require_valid(original);
    require_valid(resumed);
    CHECK(original.living().size() > 30);
}

// ---- migration from schema 1 ------------------------------------------------------------------------

namespace {

std::filesystem::path fixture_path() {
    return std::filesystem::path(PATRIARCH_TEST_DATA_DIR) / "saves" / "e1_demo_v1.psv";
}

/// The demo scenario of the fixture and of tests/golden_test.cpp.
demo::Config golden_config() {
    demo::Config config;
    config.seed = 42;
    return config;
}

}  // namespace

TEST_CASE("migration: the schema-1 fixture loads with default columns and the previous hash") {
    const auto file = read_file(fixture_path());
    REQUIRE(file.has_value());
    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());
    World& world = loaded.value().world;

    // The hash recorded in tests/golden_test.cpp when the fixture was made, part by part.
    const WorldHash hash = world.compute_hash();
    CHECK(hash.total == 0xe9822290b7ce3cc3ULL);
    CHECK(hash.calendar == 0x4807d5e946718425ULL);
    CHECK(hash.rng == 0xf263ca6972dff83cULL);
    CHECK(hash.dynasties == 0xaf4a37c18471ae17ULL);
    CHECK(hash.provinces == 0x5bb5f6e0fb36fc10ULL);
    CHECK(hash.characters == 0xc2f43c23a0a02683ULL);
    CHECK(all_ext_parts_empty(hash));
    CHECK(world.today() == Date::from_ymd(1707, 1, 1));
    CHECK_FALSE(world.dynasty_mode());
    CHECK(world.world_flags() == 0);
    CHECK(world.branches().size() == 0);
    CHECK(world.life_queue().empty());
    CHECK(world.living().size() == world.characters().ids.alive_count());
    require_valid(world);

    // Every new column holds its default.
    for (const CharacterId id : world.living()) {
        const std::size_t i = id.index();
        const CharacterTable& t = world.characters();
        CHECK_FALSE(t.name[i].valid());
        CHECK_FALSE(t.branch[i].valid());
        CHECK(t.skills[i] == std::array<Fixed32, kSkillCount>{});
        CHECK(t.traits[i] == std::array<std::uint16_t, kTraitSlots>{});
        CHECK(t.planned_death[i] == kNoDate);
        CHECK(t.death_date[i] == kNoDate);
    }
    for (std::size_t d = 0; d < world.dynasties().size(); ++d) {
        CHECK(world.dynasties().unity[d] == kDefaultUnity);
        CHECK(world.dynasties().succession_law[d] == SuccessionLaw::Majorat);
    }

    // The migrated world saves as schema 2, reloads to the same hash and the same bytes.
    const std::vector<std::uint8_t> bytes = state_bytes(world);
    auto again = load(bytes);
    REQUIRE(again.has_value());
    CHECK(again.value().compute_hash() == hash);
    CHECK(state_bytes(again.value()) == bytes);
}

TEST_CASE("migration: schema 2 differs from the schema-1 payload only by the empty extension block") {
    const auto file = read_file(fixture_path());
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
    CHECK(old_schema.u32() == 1);

    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());
    const std::vector<std::uint8_t> now = state_bytes(loaded.value().world);
    const std::size_t body_end = now.size() - 6;  // state = schema + body + empty block
    REQUIRE(old_bytes.size() >= body_end);
    CHECK(std::equal(
        old_bytes.begin() + 4, old_bytes.begin() + static_cast<std::ptrdiff_t>(body_end), now.begin() + 4));
    CHECK(std::all_of(now.end() - 6, now.end(), [](std::uint8_t b) { return b == 0; }));
    // What follows the world in the old payload (the pending internal commands) is read unchanged.
    CHECK(loaded.value().pending_internal.size() ==
          ByteReader(std::span<const std::uint8_t>(old_bytes.data() + body_end, old_bytes.size() - body_end))
              .u32());
}

TEST_CASE("migration: a loaded schema-1 save continues exactly like the uninterrupted demo") {
    const auto file = read_file(fixture_path());
    REQUIRE(file.has_value());
    auto loaded = deserialize_save(file.value());
    REQUIRE(loaded.has_value());

    Simulation reference = demo::make_simulation(golden_config());
    reference.run_days(1461);
    CHECK(loaded.value().world.compute_hash() == reference.world().compute_hash());
    reference.run_days(365);

    Simulation resumed = demo::make_simulation(golden_config());
    resumed.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
    resumed.run_days(365);
    CHECK(resumed.world().compute_hash() == reference.world().compute_hash());
    CHECK(all_ext_parts_empty(resumed.world().compute_hash()));
}

TEST_CASE("migration: a world with extended data cannot be read as an older schema, a newer one is refused") {
    std::vector<std::uint8_t> bytes = state_bytes(plain_world());
    ByteWriter tampered;
    tampered.u32(World::kStateSchema + 1);
    tampered.bytes(std::span<const std::uint8_t>(bytes.data() + 4, bytes.size() - 4));
    const auto newer = load(tampered.data());
    REQUIRE_FALSE(newer.has_value());
    CHECK(newer.error().message.find("newer") != std::string::npos);

    ByteWriter zero;
    zero.u32(0);
    zero.bytes(std::span<const std::uint8_t>(bytes.data() + 4, bytes.size() - 4));
    CHECK_FALSE(load(zero.data()).has_value());

    // Schema 1 data with a section appended is not a thing: sections need schema 2.
    ByteWriter old_with_block;
    old_with_block.u32(1);
    old_with_block.bytes(std::span<const std::uint8_t>(bytes.data() + 4, bytes.size() - 4));
    const auto trailing = load(old_with_block.data());
    CHECK_FALSE(trailing.has_value());  // six bytes remain unread
}

// ---- damaged and hostile extension blocks -----------------------------------------------------------

TEST_CASE("save v2: hand-made person section loads to the same world the API builds") {
    const std::vector<std::uint8_t> plain = state_bytes(plain_world());

    PersonRow row;
    row.id = 3;
    row.name = 77;
    row.portrait = 4;
    row.legitimacy = 2;
    row.skills[1] = raw(7);
    row.scales[1] = raw(33);   // stress
    row.scales[3] = raw(-40);  // ambition
    row.traits[0] = 5;
    row.traits[1] = 9;
    row.focus = 4;
    row.tutor = 1;
    row.level = 12;
    World expected = plain_world();
    const CharacterId id(3);
    expected.set_character_name(id, StringId(77));
    expected.set_character_portrait(id, 4);
    expected.set_character_legitimacy(id, Legitimacy::Legitimized);
    expected.set_character_skill(id, Skill::Diplomacy, fx(7));
    expected.set_character_stress(id, fx(33));
    expected.set_character_ambition(id, fx(-40));
    const std::vector<std::uint16_t> traits = {9, 5};
    REQUIRE(expected.set_character_traits(id, traits));
    REQUIRE(expected.set_character_education(id, Education{4, CharacterId(1), 12}));
    row.planned = expected.characters().birth_date[3].days() + 20000;
    REQUIRE(expected.set_character_planned_death(id, Date::from_days(row.planned)));

    auto loaded = load(with_extension(plain, 0, {person_section({row})}));
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().compute_hash() == expected.compute_hash());
    CHECK(loaded.value().characters() == expected.characters());
    CHECK(state_bytes(loaded.value()) == state_bytes(expected));
}

TEST_CASE("save v2: damaged extension blocks are rejected with an error") {
    const std::vector<std::uint8_t> plain = state_bytes(plain_world());
    auto rejected = [&](const std::vector<std::uint8_t>& bytes, const char* expected_text = nullptr) {
        const auto result = load(bytes);
        if (result.has_value()) {
            return false;
        }
        return expected_text == nullptr || result.error().message.find(expected_text) != std::string::npos;
    };
    PersonRow ok_row;
    ok_row.id = 2;
    ok_row.skills[0] = raw(5);
    CHECK_FALSE(rejected(with_extension(plain, 0, {person_section({ok_row})})));  // the control case loads

    // framing
    CHECK(rejected(with_extension(plain, 2, {})));  // unknown flag bit
    CHECK(rejected(with_extension(plain, 0x80000000U, {})));
    CHECK(rejected(with_extension(plain, 0, {}, 33)));                  // more sections than allowed
    CHECK(rejected(with_extension(plain, 0, {}, 1)));                   // declared but missing
    CHECK(rejected(with_extension(plain, 0, {RawSection{9, 1, {}}})));  // unknown section id
    CHECK(rejected(with_extension(plain, 0, {RawSection{0, 1, {}}})));  // id 0 is not a section
    RawSection wrong_version = person_section({ok_row});
    wrong_version.version = 2;
    CHECK(rejected(with_extension(plain, 0, {wrong_version})));
    wrong_version.version = 0;
    CHECK(rejected(with_extension(plain, 0, {wrong_version})));
    RawSection dynasty{2, 1, {0, 0, 0, 0, 0, 0, 0, 0}};
    CHECK_FALSE(rejected(with_extension(plain, 0, {person_section({ok_row}), dynasty})));
    CHECK(rejected(with_extension(plain, 0, {dynasty, person_section({ok_row})})));  // order
    CHECK(rejected(
        with_extension(plain, 0, {person_section({ok_row}), person_section({ok_row})})));  // repeated id
    RawSection overlong = person_section({ok_row});
    overlong.body.push_back(0);
    CHECK(rejected(with_extension(plain, 0, {overlong})));  // trailing byte inside the section
    RawSection short_body = person_section({ok_row});
    short_body.body.pop_back();
    CHECK(rejected(with_extension(plain, 0, {short_body})));  // row cut short
    RawSection empty_body{1, 1, {}};
    CHECK(rejected(with_extension(plain, 0, {empty_body})));  // no row count at all

    // a section that claims more bytes than the file has
    {
        ByteWriter w;
        w.bytes(std::span<const std::uint8_t>(plain.data(), plain.size() - 6));
        w.u32(0);
        w.u16(1);
        w.u16(1);
        w.u16(1);
        w.u32(0x7FFFFFFFU);
        CHECK(rejected(w.take()));
    }

    // row counts that are larger than the bytes behind them must not allocate or crash
    {
        ByteWriter w;
        w.u32(0xFFFFFFFFU);
        CHECK(rejected(with_extension(plain, 0, {RawSection{1, 1, w.take()}})));
        ByteWriter w2;
        w2.u32(0x01000000U);
        w2.u32(0);
        CHECK(rejected(with_extension(plain, 0, {RawSection{2, 1, w2.take()}})));
        ByteWriter w3;
        w3.u32(0);
        w3.u32(0xFFFFFFFFU);
        CHECK(rejected(with_extension(plain, 0, {RawSection{2, 1, w3.take()}})));
    }

    // person rows
    auto with_row = [&](const PersonRow& row) { return with_extension(plain, 0, {person_section({row})}); };
    PersonRow bad = ok_row;
    bad.id = 30;  // outside the table of 30 characters
    CHECK(rejected(with_row(bad)));
    bad = ok_row;
    bad.id = kNone;
    CHECK(rejected(with_row(bad)));
    CHECK(rejected(with_extension(plain, 0, {person_section({ok_row, ok_row})})));  // not ascending
    PersonRow first = ok_row;
    first.id = 5;
    CHECK(rejected(with_extension(plain, 0, {person_section({first, ok_row})})));  // descending
    bad = ok_row;
    bad.legitimacy = 3;
    CHECK(rejected(with_row(bad)));
    bad = ok_row;
    bad.skills[2] = raw(21);
    CHECK(rejected(with_row(bad), "invariants"));
    bad = ok_row;
    bad.skills[2] = raw(-1);
    CHECK(rejected(with_row(bad), "invariants"));
    for (int scale = 0; scale < 3; ++scale) {
        bad = ok_row;
        bad.scales[scale] = raw(101);  // fertility, stress, piety
        CHECK(rejected(with_row(bad), "invariants"));
        bad.scales[scale] = raw(-1);
        CHECK(rejected(with_row(bad), "invariants"));
    }
    for (int scale = 3; scale < 6; ++scale) {
        bad = ok_row;
        bad.scales[scale] = raw(101);  // ambition, loyalties
        CHECK(rejected(with_row(bad), "invariants"));
        bad.scales[scale] = raw(-101);
        CHECK(rejected(with_row(bad), "invariants"));
    }
    bad = ok_row;
    bad.traits[0] = 9;
    bad.traits[1] = 3;  // not ascending
    CHECK(rejected(with_row(bad), "invariants"));
    bad.traits[0] = 3;
    bad.traits[1] = 3;  // repeated
    CHECK(rejected(with_row(bad), "invariants"));
    bad.traits[0] = 0;
    bad.traits[1] = 3;  // gap at the front
    CHECK(rejected(with_row(bad), "invariants"));
    bad = ok_row;
    bad.focus = 6;
    CHECK(rejected(with_row(bad), "invariants"));
    bad = ok_row;
    bad.level = 101;
    CHECK(rejected(with_row(bad), "invariants"));
    bad = ok_row;
    bad.tutor = 30;  // outside the table
    CHECK(rejected(with_row(bad), "invariants"));
    bad = ok_row;
    bad.branch = 0;  // no branches exist
    CHECK(rejected(with_row(bad), "invariants"));
    bad = ok_row;
    bad.death = 1000;  // a living character with a death date
    CHECK(rejected(with_row(bad), "invariants"));
    bad = ok_row;
    bad.cause = 1;
    CHECK(rejected(with_row(bad), "invariants"));
    bad = ok_row;
    bad.planned = -900000;  // long before the birth
    CHECK(rejected(with_row(bad), "invariants"));
    bad = ok_row;
    bad.planned = std::numeric_limits<std::int32_t>::max();
    CHECK(rejected(with_row(bad), "invariants"));

    // dynasty rows and branches
    auto dynasty_section = [&](std::uint32_t dynasty_row,
                               std::int32_t unity,
                               std::uint8_t law,
                               std::uint32_t branch_dynasty,
                               bool with_branch) {
        ByteWriter w;
        w.u32(1);
        w.u32(dynasty_row);
        w.i32(unity);
        w.u8(law);
        w.u32(with_branch ? 1 : 0);
        if (with_branch) {
            w.u32(44);
            w.u32(branch_dynasty);
        }
        return RawSection{2, 1, w.take()};
    };
    CHECK_FALSE(rejected(with_extension(plain, 0, {dynasty_section(1, raw(40), 2, 2, true)})));
    CHECK(rejected(
        with_extension(plain, 0, {dynasty_section(3, raw(40), 2, 2, true)})));  // dynasty row out of range
    CHECK(rejected(with_extension(plain, 0, {dynasty_section(1, raw(101), 2, 2, false)}), "invariants"));
    CHECK(rejected(with_extension(plain, 0, {dynasty_section(1, raw(-1), 2, 2, false)}), "invariants"));
    CHECK(rejected(with_extension(plain, 0, {dynasty_section(1, raw(40), 5, 2, false)})));  // unknown law
    CHECK(rejected(
        with_extension(plain, 0, {dynasty_section(1, raw(40), 0, 3, true)})));  // branch of unknown dynasty
    CHECK(rejected(with_extension(plain, 0, {dynasty_section(1, raw(40), 0, kNone, true)})));

    // a branch that is used by a character of another dynasty
    {
        PersonRow member = ok_row;
        member.id = 1;      // character 1 belongs to dynasty 1
        member.branch = 0;  // branch 0 will belong to dynasty 2
        CHECK(rejected(
            with_extension(plain, 0, {person_section({member}), dynasty_section(1, raw(40), 0, 2, true)}),
            "invariants"));
        member.id = 2;  // character 2 belongs to dynasty 2
        CHECK_FALSE(rejected(
            with_extension(plain, 0, {person_section({member}), dynasty_section(1, raw(40), 0, 2, true)})));
    }
}

TEST_CASE("save v2: dynasty mode needs an empty free list, archived rows need dynasty mode") {
    World world(Date::from_ymd(1703, 1, 1), 1);
    const DynastyId d = world.create_dynasty(StringId(0));
    const CharacterId a = world.create_character(adult(d));
    (void)world.create_character(adult(d));
    REQUIRE(world.destroy_character(a));
    const std::vector<std::uint8_t> plain = state_bytes(world);
    CHECK(load(plain).has_value());  // E1 mode is fine
    const auto moded = load(with_extension(plain, kWorldFlagDynastyMode, {}));
    REQUIRE_FALSE(moded.has_value());
    CHECK(moded.error().message.find("free list") != std::string::npos);

    // A slot byte of 2 (archived) without the dynasty mode, the death date and the cause of an archived
    // row is refused (the archive itself is tested in archive_test.cpp).
    World two(Date::from_ymd(1703, 1, 1), 1);
    const DynastyId e = two.create_dynasty(StringId(0));
    (void)two.create_character(adult(e));
    std::vector<std::uint8_t> bytes = state_bytes(two);
    // locate the slot table: it is the unique run "capacity=1, alive=1" in the character table
    const std::vector<std::uint8_t> marker = {1, 0, 0, 0, 1};
    const auto at = std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end());
    REQUIRE(at != bytes.end());
    *(at + 4) = 2;
    CHECK_FALSE(load(bytes).has_value());
}

TEST_CASE("save v2: the character table is bounded") {
    std::vector<std::uint8_t> bytes = state_bytes(plain_world());
    // Find "capacity = 30" followed by thirty alive flags and overwrite it with an absurd value.
    std::vector<std::uint8_t> marker = {30, 0, 0, 0};
    marker.insert(marker.end(), 30, 1);
    const auto at = std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end());
    REQUIRE(at != bytes.end());
    ByteWriter huge;
    huge.u32(kMaxCharacterIds + 1);
    std::copy(huge.data().begin(), huge.data().end(), at);
    CHECK_FALSE(load(bytes).has_value());
    std::fill(at, at + 4, 0xFF);
    CHECK_FALSE(load(bytes).has_value());
}

TEST_CASE("save v2: random damage to the whole payload and to the extension block is handled safely") {
    const World rich = rich_world();
    const std::vector<std::uint8_t> payload = state_bytes(rich);
    const std::size_t tail = payload.size() - state_bytes(plain_world()).size() + 6;  // the extension block
    REQUIRE(tail > 100);
    REQUIRE(load(payload).has_value());

    auto survives_checks = [](std::span<const std::uint8_t> bytes, int& accepted, int& rejected) {
        auto result = load(bytes);
        if (!result.has_value()) {
            ++rejected;
            return;
        }
        ++accepted;
        // A mutation that still loads must describe a consistent world that saves and reloads.
        CHECK(result.value().check_invariants().empty());
        const auto again = load(state_bytes(result.value()));
        REQUIRE(again.has_value());
        CHECK(again.value().compute_hash() == result.value().compute_hash());
    };

    Pcg32 rng(2024, 9);
    int accepted = 0;
    int rejected = 0;
    for (int trial = 0; trial < 1500; ++trial) {  // anywhere
        auto mutated = payload;
        const int flips = 1 + static_cast<int>(rng.bounded(3));
        for (int f = 0; f < flips; ++f) {
            mutated[rng.bounded(static_cast<std::uint32_t>(mutated.size()))] =
                static_cast<std::uint8_t>(rng.next_u32());
        }
        survives_checks(mutated, accepted, rejected);
    }
    for (int trial = 0; trial < 3000; ++trial) {  // inside the sections only
        auto mutated = payload;
        const int flips = 1 + static_cast<int>(rng.bounded(3));
        for (int f = 0; f < flips; ++f) {
            const std::size_t at = mutated.size() - tail + rng.bounded(static_cast<std::uint32_t>(tail));
            mutated[at] = static_cast<std::uint8_t>(rng.next_u32());
        }
        survives_checks(mutated, accepted, rejected);
    }
    // Every byte of the block, set to a few extreme values.
    for (std::size_t offset = payload.size() - tail; offset < payload.size(); ++offset) {
        for (const std::uint8_t value : {std::uint8_t{0x00}, std::uint8_t{0xFF}, std::uint8_t{0x80}}) {
            if (payload[offset] == value) {
                continue;
            }
            auto mutated = payload;
            mutated[offset] = value;
            survives_checks(mutated, accepted, rejected);
        }
    }
    CHECK(rejected > 1000);
    CHECK(accepted > 0);  // some harmless changes (a name, a portrait) are legitimately accepted

    // Every proper prefix of the payload is an error; the block is mandatory in schema 2.
    for (std::size_t n = 0; n < payload.size(); n += (n + 64 < payload.size() ? 13 : 1)) {
        CHECK_FALSE(load(std::span<const std::uint8_t>(payload.data(), n)).has_value());
    }
}

// =================================================================================================
// commands: tags, CreatePerson, Die-free mode rules
// =================================================================================================

TEST_CASE("commands: tags are an explicit table, independent from the position in the variant") {
    CHECK(static_cast<int>(CommandType::SetProvinceOwner) == 1);
    CHECK(static_cast<int>(CommandType::CreateCharacter) == 2);
    CHECK(static_cast<int>(CommandType::KillCharacter) == 3);
    CHECK(static_cast<int>(CommandType::SetDynastyHead) == 4);
    CHECK(static_cast<int>(CommandType::AdjustHonor) == 5);
    CHECK(static_cast<int>(CommandType::Die) == 6);
    CHECK(static_cast<int>(CommandType::Marry) == 7);
    CHECK(static_cast<int>(CommandType::EndMarriage) == 8);
    CHECK(static_cast<int>(CommandType::CreatePerson) == 9);  // E7 block 6..31, in order of arrival
    CHECK(static_cast<int>(CommandType::TakeLoan) == 38);
    CHECK(static_cast<int>(CommandType::RepayLoan) == 39);

    const std::vector<std::pair<Command, CommandType>> cases = {
        {Command{DynastyId(), cmd::SetProvinceOwner{}}, CommandType::SetProvinceOwner},
        {Command{DynastyId(), cmd::CreateCharacter{}}, CommandType::CreateCharacter},
        {Command{DynastyId(), cmd::KillCharacter{}}, CommandType::KillCharacter},
        {Command{DynastyId(), cmd::SetDynastyHead{}}, CommandType::SetDynastyHead},
        {Command{DynastyId(), cmd::AdjustHonor{}}, CommandType::AdjustHonor},
        {Command{DynastyId(), cmd::CreatePerson{}}, CommandType::CreatePerson},
        {Command{DynastyId(), cmd::Die{CharacterId(4), CauseOfDeath::Grief}}, CommandType::Die},
        {Command{DynastyId(), cmd::Marry{CharacterId(1), CharacterId(2)}}, CommandType::Marry},
        {Command{DynastyId(), cmd::EndMarriage{CharacterId(3), MarriageEnd::Tonsure}},
         CommandType::EndMarriage},
        {Command{DynastyId(), cmd::SetTaxPolicy{DynastyId(2), 3}},
         CommandType::SetTaxPolicy},  // E9 block 32..63
        {Command{DynastyId(), cmd::SetRecruitPolicy{DynastyId(2), 1}}, CommandType::SetRecruitPolicy},
        {Command{DynastyId(), cmd::ConscriptRecruits{ProvinceId(5), 250}}, CommandType::ConscriptRecruits},
        {Command{DynastyId(), cmd::StartConstruction{ProvinceId(6), 3}}, CommandType::StartConstruction},
        {Command{DynastyId(), cmd::CancelConstruction{ProvinceId(7), 2}}, CommandType::CancelConstruction},
        {Command{DynastyId(2), cmd::TakeLoan{12'345'678'901}}, CommandType::TakeLoan},
        {Command{DynastyId(2), cmd::RepayLoan{7, 87'654'321}}, CommandType::RepayLoan},
    };
    for (const auto& [command, tag] : cases) {
        CHECK(type_of(command) == tag);
        ByteWriter w;
        write_command(w, command);
        REQUIRE_FALSE(w.data().empty());
        CHECK(w.data()[0] == static_cast<std::uint8_t>(tag));  // the first byte of a record is the tag
        CHECK(to_string(tag) != "?");
        ByteReader in(w.data());
        Command back;
        REQUIRE(read_command(in, back));
        CHECK(back == command);
        CHECK(in.at_end());
        // Any shorter record is an error, never a partial command.
        for (std::size_t n = 0; n < w.data().size(); ++n) {
            ByteReader cut(std::span<const std::uint8_t>(w.data().data(), n));
            Command ignored;
            CHECK_FALSE(read_command(cut, ignored));
        }
    }
    // Tags that nobody uses are unknown commands.
    for (const std::uint8_t tag : {std::uint8_t{0},
                                   std::uint8_t{10},
                                   std::uint8_t{11},
                                   std::uint8_t{31},
                                   std::uint8_t{63},
                                   std::uint8_t{255}}) {
        std::vector<std::uint8_t> bytes(40, 0);
        bytes[0] = tag;
        ByteReader in(bytes);
        Command ignored;
        CHECK_FALSE(read_command(in, ignored));
    }
}

namespace {

cmd::CreatePerson rich_person(const World& world) {
    cmd::CreatePerson c;
    CharacterInit& i = c.init;
    i.dynasty = DynastyId(0);
    i.birth_date = world.today();
    i.sex = Sex::Female;
    i.health = fx(90);
    i.name = StringId(321);
    i.portrait = 12;
    i.legitimacy = Legitimacy::Illegitimate;
    i.skills = {fx(1), fx(2), fx(3), fx(4), fx(20)};
    i.fertility = fx(60);
    i.stress = fx(5);
    i.piety = fx(70);
    i.ambition = fx(-20);
    i.loyalty_head = fx(10);
    i.loyalty_emperor = fx(-90);
    i.traits = {3, 8, 21, 0, 0, 0, 0, 0};
    i.planned_death = world.today() + 25000;
    return c;
}

Result<void, CommandError>
verdict_of(const World& world, const cmd::CreatePerson& c, DynastyId actor = DynastyId()) {
    return validate(world, Command{actor, c});
}

}  // namespace

TEST_CASE("commands: CreatePerson validates every column and creates the full record") {
    World world = plain_world();
    REQUIRE(world.enable_dynasty_mode());
    const BranchId branch = world.create_branch(DynastyId(0), StringId(70));
    const BranchId foreign = world.create_branch(DynastyId(1), StringId(71));
    cmd::CreatePerson good = rich_person(world);
    good.init.branch = branch;
    good.init.education = Education{2, CharacterId(0), 4};
    REQUIRE(verdict_of(world, good).has_value());

    auto rejects = [&](const cmd::CreatePerson& candidate, Rejection reason) {
        const auto result = verdict_of(world, candidate);
        return !result.has_value() && result.error().reason == reason;
    };
    CHECK(verdict_of(world, good, DynastyId(0)).error().reason == Rejection::NotAuthorized);  // system only
    cmd::CreatePerson c = good;
    c.init.dynasty = DynastyId(40);
    CHECK(rejects(c, Rejection::UnknownEntity));
    c = good;
    c.init.birth_date = world.today() + 1;
    CHECK(rejects(c, Rejection::InvalidArgument));
    c = good;
    c.init.health = fx(101);
    CHECK(rejects(c, Rejection::InvalidArgument));
    c = good;
    c.init.skills[4] = fx(21);
    CHECK(rejects(c, Rejection::InvalidArgument));
    c = good;
    c.init.stress = fx(-1);
    CHECK(rejects(c, Rejection::InvalidArgument));
    c = good;
    c.init.loyalty_emperor = fx(101);
    CHECK(rejects(c, Rejection::InvalidArgument));
    c = good;
    c.init.traits = {9, 3, 0, 0, 0, 0, 0, 0};
    CHECK(rejects(c, Rejection::InvalidArgument));
    c = good;
    c.init.education.focus = 6;
    CHECK(rejects(c, Rejection::InvalidArgument));
    c = good;
    c.init.education.tutor = CharacterId(500);
    CHECK(rejects(c, Rejection::UnknownEntity));
    c = good;
    c.init.branch = foreign;
    CHECK(rejects(c, Rejection::InvalidArgument));
    c = good;
    c.init.planned_death = world.today() - 1000000;
    CHECK(rejects(c, Rejection::InvalidArgument));
    c = good;
    c.init.father = CharacterId(1);  // a woman
    CHECK(rejects(c, Rejection::InvalidArgument));
    c = good;
    c.init.mother = CharacterId(800);
    CHECK(rejects(c, Rejection::UnknownEntity));
    c = good;
    c.init.sex = static_cast<Sex>(7);
    CHECK(rejects(c, Rejection::InvalidArgument));
    c = good;
    c.init.legitimacy = static_cast<Legitimacy>(9);
    CHECK(rejects(c, Rejection::InvalidArgument));

    const std::size_t before = world.living().size();
    const std::size_t queued = world.life_queue().size();
    apply(world, Command{DynastyId(), good});
    REQUIRE(world.living().size() == before + 1);
    const CharacterId made = world.living().back();
    const CharacterTable& t = world.characters();
    CHECK(t.name[made.index()] == StringId(321));
    CHECK(t.branch[made.index()] == branch);
    CHECK(t.legitimacy[made.index()] == Legitimacy::Illegitimate);
    CHECK(t.skills[made.index()][4] == fx(20));
    CHECK(t.traits[made.index()][2] == 21);
    CHECK(t.education_focus[made.index()] == 2);
    CHECK(t.planned_death[made.index()] == world.today() + 25000);
    CHECK(world.life_queue().size() ==
          queued + 2);  // a birthday (the birth date itself) and the planned death
    require_valid(world);
}

TEST_CASE("commands: CreatePerson survives the command log and the pending-command list of a save") {
    World world = plain_world();
    const cmd::CreatePerson person = rich_person(world);
    CommandLog log;
    const std::vector<Command> commands = {Command{DynastyId(), person},
                                           Command{DynastyId(), cmd::CreatePerson{}},
                                           Command{DynastyId(), cmd::AdjustHonor{DynastyId(1), 5}}};
    const ApplyStats stats = process_commands(world, commands, CommandSource::External, log);
    CHECK(stats.applied == 2);
    CHECK(stats.rejected == 1);  // CreatePerson{} has no dynasty
    CHECK(log.size() == 3);
    auto back = CommandLog::deserialize(log.serialize());
    REQUIRE(back.has_value());
    CHECK(back.value() == log);
    CHECK(std::get<cmd::CreatePerson>(back.value().entries()[0].command.payload) == person);

    const std::vector<std::uint8_t> save = serialize_save(world, commands);
    auto loaded = deserialize_save(save);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().pending_internal == commands);

    // Damaged records (bad sex, bad legitimacy) are refused when read.
    ByteWriter w;
    write_command(w, Command{DynastyId(), person});
    std::vector<std::uint8_t> bytes = w.take();
    auto bad_sex = bytes;
    bad_sex[1 + 4 + 4 + 4] = 5;  // tag, actor, dynasty, birth date, then sex
    ByteReader a(bad_sex);
    Command ignored;
    CHECK_FALSE(read_command(a, ignored));
}

TEST_CASE("commands: the demo's CreateCharacter and KillCharacter keep working in E1 mode only") {
    World world = plain_world();
    CHECK(validate(world, Command{DynastyId(), cmd::KillCharacter{CharacterId(3)}}).has_value());
    CHECK(validate(world,
                   Command{DynastyId(), cmd::CreateCharacter{DynastyId(0), world.today(), Sex::Male, {}, {}}})
              .has_value());
    REQUIRE(world.enable_dynasty_mode());
    CHECK_FALSE(validate(world, Command{DynastyId(), cmd::KillCharacter{CharacterId(3)}}).has_value());
    // Creation still works and schedules the new person.
    const std::size_t queued = world.life_queue().size();
    apply(world, Command{DynastyId(), cmd::CreateCharacter{DynastyId(0), world.today(), Sex::Male, {}, {}}});
    CHECK(world.life_queue().size() == queued + 1);
    require_valid(world);
}

// =================================================================================================
// a simulation with a system that uses the new columns: save in the middle, replay, divergence
// =================================================================================================

namespace {

/// Touches only extended columns (and emits CreatePerson), so the five classic hash parts do not
/// depend on it, but `total` does.
class PersonDrift final : public System {
public:
    explicit PersonDrift(int stress_step) : stress_step_(stress_step) {}

    [[nodiscard]] std::string_view name() const override { return "test.person_drift"; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }
    void run(World& world, TickContext& context) override {
        const auto living = world.living();
        if (!living.empty()) {
            const CharacterId who = living[static_cast<std::size_t>(context.tick_index) % living.size()];
            const Fixed32 now = world.characters().stress[who.index()];
            world.set_character_stress(who, now + fx(stress_step_));
        }
        if (context.tick_index % 20 == 0) {
            CharacterInit init;
            init.dynasty = DynastyId(static_cast<std::uint32_t>(context.tick_index / 20) % 3);
            init.birth_date = context.date;
            init.skills = {fx(static_cast<int>(context.tick_index % 21)), {}, {}, {}, {}};
            init.planned_death = context.date + 5000;
            context.emit(Command{DynastyId(), cmd::CreatePerson{init}});
        }
    }

private:
    int stress_step_;
};

Simulation drift_simulation(int stress_step) {
    World world(Date::from_ymd(1703, 1, 1), 21);
    REQUIRE(world.enable_dynasty_mode());
    fill_e1(world);
    Simulation sim(std::move(world));
    sim.add_system(std::make_unique<PersonDrift>(stress_step));
    return sim;
}

}  // namespace

TEST_CASE("simulation: a dynasty-mode world saved with a pending CreatePerson resumes identically") {
    Simulation original = drift_simulation(1);
    original.run_days(40);  // tick 40 emitted a CreatePerson that has not been applied yet
    REQUIRE_FALSE(original.pending_internal().empty());
    REQUIRE(std::holds_alternative<cmd::CreatePerson>(original.pending_internal()[0].payload));

    const auto bytes = serialize_save(original.world(), original.pending_internal());
    Simulation resumed = drift_simulation(1);
    auto loaded = deserialize_save(bytes);
    REQUIRE(loaded.has_value());
    resumed.restore_state(std::move(loaded.value().world), std::move(loaded.value().pending_internal));
    CHECK(resumed.world().compute_hash() == original.world().compute_hash());

    original.run_days(300);
    resumed.run_days(300);
    CHECK(resumed.world().compute_hash() == original.world().compute_hash());
    CHECK(resumed.world().characters() == original.world().characters());
    CHECK(original.world().living().size() > 30);
    CHECK(original.world().check_invariants().empty());
    CHECK(resumed.world().check_invariants().empty());
}

TEST_CASE("simulation: replays reproduce extended state and name the extended part that diverges") {
    Simulation recorded = drift_simulation(1);
    ReplayRecorder recorder(21, 0x7E57ULL, recorded.world().today(), 30);
    for (int day = 0; day < 120; ++day) {
        if (day == 50) {
            REQUIRE(recorded.submit(Command{DynastyId(), rich_person(recorded.world())}));
        }
        (void)recorded.tick();
        recorder.after_tick(recorded);
    }
    const Replay replay = recorder.finish(recorded);
    REQUIRE(std::any_of(replay.commands.begin(), replay.commands.end(), [](const ReplayCommand& c) {
        return std::holds_alternative<cmd::CreatePerson>(c.command.payload);
    }));

    // The file survives serialization (the external CreatePerson is in it).
    auto parsed = deserialize_replay(serialize_replay(replay));
    REQUIRE(parsed.has_value());

    Simulation same = drift_simulation(1);
    const ReplayResult ok = play_replay(same, parsed.value());
    CHECK(ok.ok);
    CHECK(same.world().compute_hash() == recorded.world().compute_hash());

    // Same five classic parts, but a different stress step: only `total` (which covers the
    // extended parts) shows it, and the report says that the cause is an extended part.
    Simulation different = drift_simulation(2);
    const ReplayResult bad = play_replay(different, parsed.value());
    REQUIRE_FALSE(bad.ok);
    REQUIRE(bad.divergence.has_value());
    CHECK(bad.divergence->first_differing_part.find("extended parts") == 0);
    CHECK(bad.divergence->first_differing_part.find("character_ext") != std::string::npos);
    CHECK(bad.divergence->expected.dynasties == bad.divergence->actual.dynasties);
    CHECK(bad.divergence->expected.characters == bad.divergence->actual.characters);
    CHECK(bad.divergence->expected.total != bad.divergence->actual.total);

    // Two fully computed hashes name the differing extended part.
    WorldHash a;
    WorldHash b;
    a.ext[ext_index(ExtPart::DynastyExt)] = 1;
    b.ext[ext_index(ExtPart::DynastyExt)] = 2;
    CHECK(first_differing_part(a, b) == "dynasty_ext");
    CHECK(first_differing_part(a, a).empty());
}
