#include "pat/sim/kinship.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

using namespace pat::sim;
using pat::core::StringId;

namespace {

World make_world() {
    World world(Date::from_ymd(1703, 1, 1), 7);
    REQUIRE(world.enable_dynasty_mode());
    (void)world.create_dynasty(StringId(0));
    return world;
}

CharacterId person(World& world, Sex sex, int birth_year, CharacterId father = {}, CharacterId mother = {}) {
    CharacterInit init;
    init.dynasty = DynastyId(0);
    init.sex = sex;
    init.birth_date = Date::from_ymd(birth_year, 1, 1);
    init.father = father;
    init.mother = mother;
    const auto id = world.create_character(init);
    REQUIRE(id.valid());
    return id;
}

void blood_both(
    const World& world, CharacterId a, CharacterId b, std::uint8_t degree, std::size_t generations = 6) {
    CHECK(kin::blood_degree(world, a, b, generations) == degree);
    CHECK(kin::blood_degree(world, b, a, generations) == degree);
}

void affinity_both(const World& world, CharacterId a, CharacterId b, std::uint8_t degree) {
    CHECK(kin::affinity_degree(world, a, b) == degree);
    CHECK(kin::affinity_degree(world, b, a) == degree);
}

void spiritual_both(const World& world, CharacterId a, CharacterId b, bool related) {
    CHECK(kin::spiritually_related(world, a, b) == related);
    CHECK(kin::spiritually_related(world, b, a) == related);
}

}  // namespace

TEST_CASE("marriage kinship: Roman degrees include half siblings and cousins") {
    auto world = make_world();
    const auto grandfather = person(world, Sex::Male, 1620);
    const auto grandmother = person(world, Sex::Female, 1621);
    const auto other_grandmother = person(world, Sex::Female, 1622);
    const auto father = person(world, Sex::Male, 1645, grandfather, grandmother);
    const auto aunt = person(world, Sex::Female, 1647, grandfather, grandmother);
    const auto half_uncle = person(world, Sex::Male, 1649, grandfather, other_grandmother);
    const auto cousin_a = person(world, Sex::Male, 1670, father);
    const auto cousin_b = person(world, Sex::Female, 1671, {}, aunt);
    const auto child_a = person(world, Sex::Female, 1691, cousin_a);
    const auto child_b = person(world, Sex::Male, 1690, {}, cousin_b);
    const auto unrelated = person(world, Sex::Female, 1672);

    blood_both(world, father, cousin_a, 1);
    blood_both(world, father, aunt, 2);
    blood_both(world, father, half_uncle, 2);
    blood_both(world, aunt, cousin_a, 3);
    blood_both(world, cousin_a, cousin_b, 4);
    blood_both(world, cousin_a, child_b, 5);
    blood_both(world, child_a, child_b, 6);
    blood_both(world, cousin_a, unrelated, 0);
    blood_both(world, cousin_a, cousin_b, 0, 1);
    blood_both(world, cousin_a, cousin_b, 4, 2);
    blood_both(world, father, cousin_a, 0, 0);
    CHECK(world.check_invariants().empty());
}

TEST_CASE("marriage kinship: six generations cover direct and unbalanced lines") {
    auto world = make_world();
    std::array<CharacterId, 8> line{};
    line[0] = person(world, Sex::Male, 1560);
    for (std::size_t i = 1; i < line.size(); ++i) {
        line[i] = person(world, Sex::Male, 1560 + static_cast<int>(i) * 20, line[i - 1]);
    }
    const auto side_branch = person(world, Sex::Female, 1582, line[0]);

    blood_both(world, line[0], line[4], 4);
    blood_both(world, line[0], line[5], 5);
    blood_both(world, line[0], line[6], 6);
    blood_both(world, side_branch, line[4], 5);  // 1 + 4.
    blood_both(world, side_branch, line[5], 6);  // 1 + 5.
    blood_both(world, side_branch, line[6], 7);  // The caller applies the forbidden-degree threshold.
    blood_both(world, line[0], line[4], 0, 3);
    blood_both(world, line[0], line[7], 0);
    blood_both(world, line[0], line[7], 0, std::numeric_limits<std::size_t>::max());
    CHECK(world.check_invariants().empty());
}

TEST_CASE("marriage kinship: repeated ancestors use the shortest path and survive archiving") {
    auto world = make_world();
    const auto ancestor = person(world, Sex::Female, 1620);
    const auto daughter = person(world, Sex::Female, 1640, {}, ancestor);
    const auto grandson = person(world, Sex::Male, 1660, {}, daughter);
    // The same ancestor is three steps up the paternal path and one step up the maternal path.
    const auto left = person(world, Sex::Male, 1690, grandson, ancestor);
    const auto right = person(world, Sex::Female, 1691, {}, ancestor);
    blood_both(world, left, right, 2);

    REQUIRE(world.die(ancestor, CauseOfDeath::OldAge));
    REQUIRE(world.die(right, CauseOfDeath::OldAge));
    blood_both(world, left, right, 2);
    blood_both(world, ancestor, left, 1);
    CHECK(world.check_invariants().empty());
}

TEST_CASE("marriage kinship: unknown people and bounded damaged ancestry are safe") {
    auto world = make_world();
    const auto a = person(world, Sex::Male, 1670);
    const auto b = person(world, Sex::Female, 1671);
    const CharacterId unknown(1000);
    blood_both(world, a, a, 0);
    blood_both(world, a, {}, 0);
    blood_both(world, a, unknown, 0);

    // Deliberate damage bypasses World: a cycle and a stale parent may not hang or index out of range.
    auto& people = const_cast<CharacterTable&>(world.characters());
    people.father[a.index()] = a;
    people.mother[a.index()] = unknown;
    blood_both(world, a, b, 0, std::numeric_limits<std::size_t>::max());

    World e1(Date::from_ymd(1703, 1, 1), 7);
    (void)e1.create_dynasty(StringId(0));
    const auto e1_a = person(e1, Sex::Male, 1670);
    const auto e1_b = person(e1, Sex::Female, 1671, e1_a);
    blood_both(e1, e1_a, e1_b, 0);
    affinity_both(e1, e1_a, e1_b, 0);
    spiritual_both(e1, e1_a, e1_b, false);
}

TEST_CASE("marriage kinship: affinity checks both people's current and former spouses") {
    auto world = make_world();
    const auto father = person(world, Sex::Male, 1640);
    const auto wife = person(world, Sex::Female, 1670, father);
    const auto brother = person(world, Sex::Male, 1672, father);
    const auto husband = person(world, Sex::Male, 1671);
    REQUIRE(world.marry(husband, wife));
    affinity_both(world, husband, brother, 2);
    REQUIRE(world.die(wife, CauseOfDeath::OldAge));
    affinity_both(world, husband, brother, 2);

    const auto other_father = person(world, Sex::Male, 1642);
    const auto other_husband = person(world, Sex::Male, 1673, other_father);
    const auto other_sibling = person(world, Sex::Female, 1674, other_father);
    const auto other_wife = person(world, Sex::Female, 1675);
    REQUIRE(world.marry(other_husband, other_wife));
    affinity_both(world, other_sibling, other_wife, 2);
    REQUIRE(world.end_marriage(other_wife));
    affinity_both(world, other_sibling, other_wife, 2);
    CHECK(world.check_invariants().empty());
}

TEST_CASE("marriage kinship: affinity returns the nearest spouse relation") {
    auto world = make_world();
    const auto grandfather = person(world, Sex::Male, 1610);
    const auto father_a = person(world, Sex::Male, 1640, grandfather);
    const auto father_b = person(world, Sex::Male, 1642, grandfather);
    const auto relative = person(world, Sex::Male, 1670, father_a);
    const auto sister = person(world, Sex::Female, 1672, father_a);
    const auto cousin = person(world, Sex::Female, 1674, father_b);
    const auto husband = person(world, Sex::Male, 1671);
    REQUIRE(world.marry(husband, cousin));
    affinity_both(world, husband, relative, 4);
    REQUIRE(world.end_marriage(husband));
    REQUIRE(world.marry(husband, sister));
    affinity_both(world, husband, relative, 2);
    REQUIRE(world.end_marriage(husband));
    affinity_both(world, husband, relative, 2);
    CHECK(kin::affinity_degree(world, husband, relative, 0) == 0);
    CHECK(world.check_invariants().empty());
}

TEST_CASE("marriage kinship: affinity ignores empty and stale spouse slots") {
    auto world = make_world();
    const auto a = person(world, Sex::Male, 1670);
    const auto b = person(world, Sex::Female, 1671);
    affinity_both(world, a, b, 0);
    affinity_both(world, a, {}, 0);
    affinity_both(world, a, CharacterId(1000), 0);
    affinity_both(world, a, a, 0);

    auto& people = const_cast<CharacterTable&>(world.characters());
    people.spouse[a.index()] = CharacterId(1000);
    people.ex_spouses[a.index()][0] = CharacterId(1001);
    people.ex_spouses[b.index()][0] = CharacterId(1002);
    affinity_both(world, a, b, 0);
}

TEST_CASE("marriage kinship: spiritual relations cover all three symmetric patterns") {
    auto world = make_world();
    const auto father = person(world, Sex::Male, 1660);
    const auto mother = person(world, Sex::Female, 1661);
    const auto godfather = person(world, Sex::Male, 1658);
    const auto godmother = person(world, Sex::Female, 1659);
    const auto other_godparent = person(world, Sex::Male, 1657);
    const auto child = person(world, Sex::Male, 1690, father, mother);
    const auto shared = person(world, Sex::Female, 1691);
    const auto unrelated = person(world, Sex::Female, 1692);
    REQUIRE(world.set_character_godparents(child, {godfather, godmother}));
    REQUIRE(world.set_character_godparents(shared, {godmother, {}}));
    REQUIRE(world.set_character_godparents(unrelated, {other_godparent, {}}));

    spiritual_both(world, godfather, child, true);
    spiritual_both(world, godfather, father, true);
    spiritual_both(world, godmother, mother, true);
    spiritual_both(world, child, shared, true);
    spiritual_both(world, child, unrelated, false);
    spiritual_both(world, father, child, false);
    spiritual_both(world, godfather, godmother, false);  // Not a fourth, co-godparent pattern.
    spiritual_both(world, child, child, false);
    spiritual_both(world, child, {}, false);
    spiritual_both(world, child, CharacterId(1000), false);

    REQUIRE(world.die(godmother, CauseOfDeath::OldAge));
    REQUIRE(world.die(child, CauseOfDeath::OldAge));
    spiritual_both(world, godmother, child, true);
    spiritual_both(world, godmother, mother, true);
    spiritual_both(world, child, shared, true);
    CHECK(world.check_invariants().empty());
}
