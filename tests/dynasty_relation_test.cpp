#include "pat/sim/dynasty_relation.hpp"
#include "pat/sim/marriage_score.hpp"

#include <doctest/doctest.h>

#include <cstdint>

namespace {

using namespace pat::sim;
using pat::core::Fixed32;
using pat::sim::economy::Breakdown;

struct RelationScene {
    World world{Date::from_ymd(1703, 1, 1), 7};
    DynastyId a;
    DynastyId b;
    DynastyId third;
    CharacterId head_a;
    CharacterId head_b;
    CharacterId member_a;
    CharacterId member_b;

    RelationScene() {
        REQUIRE(world.enable_dynasty_mode());
        a = world.create_dynasty({});
        b = world.create_dynasty({});
        third = world.create_dynasty({});
        head_a = person(a);
        head_b = person(b);
        member_a = person(a);
        member_b = person(b);
        REQUIRE(world.set_dynasty_head(a, head_a));
        REQUIRE(world.set_dynasty_head(b, head_b));
    }

    CharacterId person(DynastyId house) {
        CharacterInit init;
        init.dynasty = house;
        init.birth_date = Date::from_ymd(1670, 1, 1);
        const CharacterId id = world.create_character(init);
        REQUIRE(id.valid());
        return id;
    }

    void opinion(CharacterId from, CharacterId to, std::int32_t value) {
        REQUIRE(world.add_opinion(from, to, Fixed32::from_int(value), 1));
    }
};

std::int64_t term_value(const Breakdown& explanation, std::uint16_t code) {
    const auto node = explanation.find(code);
    REQUIRE(node != economy::kNoNode);
    return explanation.node(node).value;
}

}  // namespace

TEST_CASE("dynasty relation: directed stored opinions are symmetric and averaged before truncation") {
    RelationScene s;
    // Three directed rows; absent reverse rows do not dilute the mean.
    REQUIRE(s.world.add_opinion(s.member_a, s.member_b, Fixed32::from_ratio(29, 10), 1));
    REQUIRE(s.world.add_opinion(s.member_b, s.member_a, Fixed32::from_ratio(29, 10), 1));
    REQUIRE(s.world.add_opinion(s.head_a, s.member_b, Fixed32::from_ratio(9, 10), 1));
    CHECK(dynasty_relation(s.world, s.a, s.b) == 2);
    CHECK(dynasty_relation(s.world, s.b, s.a) == 2);

    // A zero row resulting from cancelling sources contributes neither value nor count.
    s.opinion(s.member_a, s.head_b, 20);
    REQUIRE(s.world.add_opinion(s.member_a, s.head_b, Fixed32::from_int(-20), 2));
    CHECK(dynasty_relation(s.world, s.a, s.b) == 2);

    RelationScene negative;
    REQUIRE(
        negative.world.add_opinion(negative.member_a, negative.member_b, Fixed32::from_ratio(-19, 10), 1));
    REQUIRE(negative.world.add_opinion(negative.member_b, negative.member_a, Fixed32::from_ratio(-9, 10), 1));
    CHECK(dynasty_relation(negative.world, negative.a, negative.b) == -1);
}

TEST_CASE("dynasty relation: only current heads are excluded and living house membership is used") {
    RelationScene s;
    s.opinion(s.head_a, s.head_b, 100);
    s.opinion(s.head_b, s.head_a, -80);
    CHECK(dynasty_relation(s.world, s.a, s.b) == 0);
    s.opinion(s.member_a, s.member_b, 30);
    s.opinion(s.head_b, s.member_a, 10);
    s.opinion(s.member_a, s.person(s.third), -100);
    const CharacterId dead_target = s.person(s.b);
    const CharacterId dead_holder = s.person(s.a);
    s.opinion(s.member_a, dead_target, -100);
    s.opinion(dead_holder, s.member_b, -100);
    REQUIRE(s.world.die(dead_target, CauseOfDeath::Illness));
    REQUIRE(s.world.die(dead_holder, CauseOfDeath::Illness));
    CHECK(dynasty_relation(s.world, s.a, s.b) == 20);
    CHECK(dynasty_relation(s.world, s.b, s.a) == 20);

    // Moving a target into another house changes which directed rows qualify.
    s.world.set_character_dynasty(s.member_b, s.third);
    CHECK(dynasty_relation(s.world, s.a, s.b) == 10);

    // The old heads' rows become ordinary opinions when neither is the current head.
    REQUIRE(s.world.set_dynasty_head(s.a, s.member_a));
    REQUIRE(s.world.set_dynasty_head(s.b, s.person(s.b)));
    CHECK(dynasty_relation(s.world, s.a, s.b) == 10);  // (100 - 80 + 10) / 3
}

TEST_CASE("dynasty relation: acting regent and caretaker opinions are excluded") {
    RelationScene s;
    SUBCASE("regent") {
        CharacterInit init;
        init.dynasty = s.a;
        init.birth_date = Date::from_ymd(1695, 1, 1);
        const auto ward = s.world.create_character(init);
        REQUIRE(ward.valid());
        REQUIRE(s.world.set_dynasty_head(s.a, ward));
        family::Regency row;
        row.dynasty = s.a;
        row.ward = ward;
        row.regent = s.member_a;
        row.kind = family::RegencyKind::Council;
        row.started = s.world.today();
        REQUIRE(s.world.set_family_regency(row));
    }
    SUBCASE("caretaker") {
        REQUIRE(s.world.die(s.head_a, CauseOfDeath::Illness));
        REQUIRE(s.world.set_dynasty_head(s.a, {}));
        family::Dispute row;
        row.dynasty = s.a;
        row.predecessor = s.head_a;
        row.opened = s.world.today();
        row.resolve_by = s.world.today() + 90;
        row.claimants_n = 2;
        row.claimants[0] = {s.member_a, 30};
        row.claimants[1] = {s.person(s.a), 20};
        REQUIRE(s.world.set_family_dispute(row));
    }
    s.opinion(s.member_a, s.head_b, 100);
    s.opinion(s.head_b, s.member_a, -80);
    s.opinion(s.member_a, s.member_b, 30);
    CHECK(dynasty_relation(s.world, s.a, s.b) == 30);
    CHECK(dynasty_relation(s.world, s.b, s.a) == 30);
    CHECK(s.world.check_invariants().empty());
}

TEST_CASE("dynasty relation: current marriages count once, are capped and end with death or separation") {
    RelationScene s;
    REQUIRE(s.world.marry(s.member_a, s.member_b));
    CHECK(dynasty_relation(s.world, s.a, s.b) == 5);
    CHECK(dynasty_relation(s.world, s.b, s.a) == 5);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(s.world.marry(s.person(s.a), s.person(s.b)));
    }
    CHECK(dynasty_relation(s.world, s.a, s.b) == 15);
    MarriageRules rules = MarriageRules::defaults();
    rules.relation.marriage_tie_bonus = 7;
    rules.relation.max_marriage_ties = 2;
    CHECK(dynasty_relation(s.world, s.a, s.b, rules) == 14);
    rules.relation.max_marriage_ties = 0;
    CHECK(dynasty_relation(s.world, s.a, s.b, rules) == 0);

    RelationScene ended;
    REQUIRE(ended.world.marry(ended.member_a, ended.member_b));
    REQUIRE(ended.world.end_marriage(ended.member_a));
    CHECK(dynasty_relation(ended.world, ended.a, ended.b) == 0);
    const CharacterId new_spouse = ended.person(ended.b);
    REQUIRE(ended.world.marry(ended.member_a, new_spouse));
    REQUIRE(ended.world.die(new_spouse, CauseOfDeath::Illness));
    CHECK(dynasty_relation(ended.world, ended.a, ended.b) == 0);
    REQUIRE(ended.world.die(ended.member_a, CauseOfDeath::Illness));
    CHECK(dynasty_relation(ended.world, ended.a, ended.b) == 0);
}

TEST_CASE("dynasty relation: bounded total and additive explanation preserve full capped tie bonus") {
    RelationScene s;
    s.opinion(s.member_a, s.member_b, 100);
    REQUIRE(s.world.marry(s.member_a, s.member_b));
    Breakdown explanation;
    const MarriageRules rules = MarriageRules::defaults();
    CHECK(dynasty_relation(s.world, s.a, s.b, rules, &explanation) == 100);
    CHECK(explanation.root().code == marriage_term::kRelation);
    CHECK(explanation.root().unit == economy::Unit::Points);
    CHECK(explanation.total() == Fixed32::from_int(100).raw());
    CHECK(term_value(explanation, marriage_term::kRelationOpinionMean) == Fixed32::from_int(100).raw());
    CHECK(term_value(explanation, marriage_term::kRelationMarriageTies) == Fixed32::from_int(5).raw());
    CHECK(term_value(explanation, marriage_term::kRelationLimit) == Fixed32::from_int(-5).raw());
    CHECK(explanation.is_consistent());
    CHECK_FALSE(explanation.truncated());

    RelationScene negative;
    negative.opinion(negative.member_a, negative.member_b, -100);
    CHECK(dynasty_relation(negative.world, negative.a, negative.b, rules, &explanation) == -100);
    CHECK(explanation.find(marriage_term::kRelationLimit) == economy::kNoNode);
    CHECK(explanation.is_consistent());
    REQUIRE(negative.world.marry(negative.member_a, negative.member_b));
    CHECK(dynasty_relation(negative.world, negative.a, negative.b) == -95);
}

TEST_CASE("dynasty relation: invalid and identical houses clear the explanation and return zero") {
    RelationScene s;
    s.opinion(s.member_a, s.member_b, 42);
    const MarriageRules rules = MarriageRules::defaults();
    Breakdown explanation;
    REQUIRE(dynasty_relation(s.world, s.a, s.b, rules, &explanation) == 42);
    CHECK(dynasty_relation(s.world, s.a, s.a, rules, &explanation) == 0);
    CHECK(explanation.total() == 0);
    CHECK(explanation.is_consistent());
    CHECK(term_value(explanation, marriage_term::kRelationOpinionMean) == 0);
    CHECK(term_value(explanation, marriage_term::kRelationMarriageTies) == 0);
    CHECK(dynasty_relation(s.world, {}, s.b) == 0);
    CHECK(dynasty_relation(s.world, s.a, DynastyId(999)) == 0);
    World plain{Date::from_ymd(1703, 1, 1), 7};
    const DynastyId a = plain.create_dynasty({});
    const DynastyId b = plain.create_dynasty({});
    CHECK(dynasty_relation(plain, a, b, rules, &explanation) == 0);
    CHECK(explanation.total() == 0);
    CHECK(explanation.is_consistent());
}

TEST_CASE("dynasty relation: repeated evaluation preserves the world hash and random streams") {
    RelationScene s;
    s.opinion(s.member_a, s.member_b, -13);
    REQUIRE(s.world.marry(s.member_a, s.member_b));
    const WorldHash before = s.world.compute_hash();
    Breakdown explanation;
    const MarriageRules rules = MarriageRules::defaults();
    CHECK(dynasty_relation(s.world, s.a, s.b, rules, &explanation) == -8);
    CHECK(dynasty_relation(s.world, s.b, s.a) == -8);
    CHECK(s.world.compute_hash() == before);
    CHECK(s.world.compute_hash().rng == before.rng);
    CHECK(explanation.is_consistent());
}
