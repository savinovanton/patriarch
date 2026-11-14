// Evaluation of a match (E8-T05, docs/dev/marriage-succession-model.md sections 8.3 and 8.4): the
// eight terms of accept_score, the decision and its refusal reason, the corridors of the dowry and of
// the political gain, the Breakdown, the dowry and the cost of the envoy, and the rules.

#include "pat/sim/marriage_rules.hpp"
#include "pat/sim/marriage_score.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {

using namespace pat::sim;
using pat::core::Fixed32;
using pat::sim::economy::Breakdown;
using pat::sim::economy::Kopecks;

constexpr std::size_t idx(ScoreTerm term) {
    return static_cast<std::size_t>(term);
}

constexpr Fixed32 points(int value) {
    return Fixed32::from_int(value);
}

/// What a scene is made of: the honor of the two houses and the qualities of the two people. The
/// groom is born `groom_age` years before 1 January 1703, the bride `bride_age` years before.
struct SceneParams {
    std::int32_t groom_honor = 0;
    std::int32_t bride_honor = 0;
    int groom_age = 25;
    int bride_age = 20;
    int groom_health = 50;
    int bride_health = 50;
    int bride_fertility = 50;
    int groom_skills = 8;  // each of the five skills; the sum is 5x
    int bride_skills = 8;
    int opinion = 0;  // opinion of the head of each house about the head of the proposing house
};

/// Three houses: the groom's, the bride's and a proposer (the groom's house sends the envoy unless a
/// test says otherwise). Every house has a head so that opinions can be held.
struct Scene {
    World world{Date::from_ymd(1703, 1, 1), 7};
    DynastyId groom_house;
    DynastyId bride_house;
    DynastyId third_house;
    CharacterId groom;
    CharacterId bride;
    CharacterId groom_head;
    CharacterId bride_head;
    CharacterId third_head;

    CharacterId make(DynastyId house, Sex sex, int age, int health, int skill, int fertility = 0) {
        CharacterInit init;
        init.dynasty = house;
        init.sex = sex;
        init.birth_date = Date::from_ymd(1703 - age, 1, 1);
        init.health = points(health);
        init.fertility = points(fertility);
        init.skills.fill(points(skill));
        const auto id = world.create_character(init);
        REQUIRE(id.valid());
        return id;
    }

    explicit Scene(const SceneParams& p = {}) {
        REQUIRE(world.enable_dynasty_mode());
        groom_house = world.create_dynasty({});
        bride_house = world.create_dynasty({});
        third_house = world.create_dynasty({});
        groom = make(groom_house, Sex::Male, p.groom_age, p.groom_health, p.groom_skills);
        bride =
            make(bride_house, Sex::Female, p.bride_age, p.bride_health, p.bride_skills, p.bride_fertility);
        groom_head = make(groom_house, Sex::Male, 50, 50, 8);
        bride_head = make(bride_house, Sex::Male, 50, 50, 8);
        third_head = make(third_house, Sex::Male, 50, 50, 8);
        REQUIRE(world.set_dynasty_head(groom_house, groom_head));
        REQUIRE(world.set_dynasty_head(bride_house, bride_head));
        REQUIRE(world.set_dynasty_head(third_house, third_head));
        world.add_honor(groom_house, p.groom_honor);
        world.add_honor(bride_house, p.bride_honor);
        if (p.opinion != 0) {
            REQUIRE(world.add_opinion(groom_head, third_head, points(p.opinion), 1));
            REQUIRE(world.add_opinion(bride_head, third_head, points(p.opinion), 1));
        }
    }

    [[nodiscard]] MatchEvaluation
    evaluate(const MarriageRules& rules, Kopecks dowry, MatchContext context = {}) const {
        const auto result = evaluate_match(world, rules, groom, bride, dowry, context);
        REQUIRE(result.has_value());
        return *result;
    }
};

SceneParams with_bride_honor(std::int32_t honor) {
    SceneParams p;
    p.bride_honor = honor;
    return p;
}

/// A neutral scene: equal houses, both people at the neutral values of every quality, ages 25 and 20.
SceneParams neutral() {
    SceneParams p;
    p.groom_skills = 8;  // the sum of five skills = 40
    p.bride_skills = 8;
    return p;
}

constexpr Kopecks kStandardDowry = 27'000;  // honor 340: 100 + 170 roubles

}  // namespace

// ---- rules -------------------------------------------------------------------------------------

TEST_CASE("marriage rules: the defaults are valid and match the specification table") {
    const MarriageRules rules = MarriageRules::defaults();
    CHECK(validate(rules).has_value());
    CHECK(rules == MarriageRules::defaults());
    CHECK(rules.ranks.min_honor == std::array<std::int32_t, kMarriageRankCount>{-1000, 0, 100, 500, 1000});
    CHECK(rules.score.honor_weight_permille == 150);
    CHECK(rules.score.honor_gap_limit == 600);
    CHECK(rules.score.rank_step_penalty == 40);
    CHECK(rules.score.dowry_points_per_100rub == 4);
    CHECK(rules.score.relation_weight_permille == 500);
    CHECK(rules.score.opinion_weight_permille == 500);
    CHECK(rules.score.age_gap_penalty == 8);
    CHECK(rules.score.age_gap_free_years == 10);
    CHECK(rules.score.accept_threshold == 0);
    CHECK(rules.score.forecast_margin == 15);
    CHECK(rules.money.dowry_base_kop == 10'000);
    CHECK(rules.money.dowry_per_honor_kop == 50);
    CHECK(rules.money.dowry_min_kop == 5'000);
    CHECK(rules.money.dowry_max_kop == 100'000);
    CHECK(rules.money.envoy_road_kop == 8'000);
    CHECK(rules.money.envoy_gifts_kop == 4'000);
    CHECK(rules.money.dispensation_fee_kop == 30'000);
}

TEST_CASE("marriage rules: validate rejects out-of-range and inconsistent values") {
    const MarriageRules good = MarriageRules::defaults();

    auto broken = good;
    broken.ranks.min_honor[3] = broken.ranks.min_honor[2];
    CHECK_FALSE(validate(broken).has_value());

    broken = good;
    broken.money.dowry_min_kop = broken.money.dowry_max_kop + 1;
    CHECK_FALSE(validate(broken).has_value());

    broken = good;
    broken.score.honor_weight_permille = -1;
    CHECK_FALSE(validate(broken).has_value());

    broken = good;
    broken.score.dowry_points_per_100rub = 1'000'000;
    CHECK_FALSE(validate(broken).has_value());

    broken = good;
    broken.score.health_neutral = 101;
    CHECK_FALSE(validate(broken).has_value());

    broken = good;
    broken.money.envoy_road_kop = -5;
    CHECK_FALSE(validate(broken).has_value());

    const auto error = validate(broken);
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error().message.find("envoy_road_kop") != std::string::npos);
}

TEST_CASE("marriage rules: honor ranks follow the thresholds") {
    const MarriageRules rules = MarriageRules::defaults();
    CHECK(honor_rank(rules, -1000) == 0);
    CHECK(honor_rank(rules, -1) == 0);
    CHECK(honor_rank(rules, 0) == 1);
    CHECK(honor_rank(rules, 99) == 1);
    CHECK(honor_rank(rules, 100) == 2);
    CHECK(honor_rank(rules, 499) == 2);
    CHECK(honor_rank(rules, 500) == 3);
    CHECK(honor_rank(rules, 999) == 3);
    CHECK(honor_rank(rules, 1000) == 4);
    CHECK(honor_rank(rules, 10000) == 4);
    CHECK(honor_rank(rules, -5000) == 0);  // below the first threshold is still the lowest rank
}

// ---- dowry and envoy ---------------------------------------------------------------------------

TEST_CASE("dowry: base plus honor, inside the corridor, explained") {
    const MarriageRules rules = MarriageRules::defaults();
    Scene s(with_bride_honor(340));
    Breakdown explain;
    CHECK(standard_dowry(s.world, rules, s.bride_house, &explain) == kStandardDowry);
    CHECK(explain.root().code == marriage_term::kDowryTotal);
    CHECK(explain.total() == kStandardDowry);
    CHECK(explain.node(explain.find(marriage_term::kDowryBase)).value == 10'000);
    CHECK(explain.node(explain.find(marriage_term::kDowryHonor)).value == 17'000);
    CHECK(explain.find(marriage_term::kDowryLimit) == pat::sim::economy::kNoNode);  // nothing clamped
    CHECK(explain.is_consistent());

    // The explanation never changes the answer.
    CHECK(standard_dowry(s.world, rules, s.bride_house) == kStandardDowry);

    // A negative honor adds nothing; the dowry is never below the base (here also the minimum).
    Scene poor(with_bride_honor(-400));
    CHECK(standard_dowry(poor.world, rules, poor.bride_house) == 10'000);

    // A great honor hits the maximum, and the clamp is a term of the breakdown.
    Scene rich(with_bride_honor(5000));
    Breakdown clamped;
    CHECK(standard_dowry(rich.world, rules, rich.bride_house, &clamped) == 100'000);
    CHECK(clamped.node(clamped.find(marriage_term::kDowryLimit)).value < 0);
    CHECK(clamped.is_consistent());

    // A raised minimum is a clamp upward.
    auto rules_min = rules;
    rules_min.money.dowry_min_kop = 20'000;
    Breakdown raised;
    CHECK(standard_dowry(poor.world, rules_min, poor.bride_house, &raised) == 20'000);
    CHECK(raised.node(raised.find(marriage_term::kDowryLimit)).value == 10'000);
    CHECK(raised.is_consistent());

    // An unknown house pays the base.
    CHECK(standard_dowry(s.world, rules, DynastyId{}) == 10'000);
    CHECK(standard_dowry(s.world, rules, DynastyId(99)) == 10'000);
}

TEST_CASE("envoy: road, gifts and the dispensation of the Church") {
    const MarriageRules rules = MarriageRules::defaults();
    Breakdown plain;
    CHECK(envoy_cost(rules, false, &plain) == 12'000);
    CHECK(plain.root().code == marriage_term::kEnvoyTotal);
    CHECK(plain.node(plain.find(marriage_term::kEnvoyRoad)).value == 8'000);
    CHECK(plain.node(plain.find(marriage_term::kEnvoyGifts)).value == 4'000);
    CHECK(plain.node(plain.find(marriage_term::kEnvoyDispensation)).value == 0);
    CHECK(plain.is_consistent());

    Breakdown dispensed;
    CHECK(envoy_cost(rules, true, &dispensed) == 42'000);
    CHECK(dispensed.node(dispensed.find(marriage_term::kEnvoyDispensation)).value == 30'000);
    CHECK(dispensed.is_consistent());
    CHECK(envoy_cost(rules, true) == 42'000);
}

// ---- the terms ---------------------------------------------------------------------------------

TEST_CASE("accept_score: the neutral match gives the dowry and nothing else") {
    const MarriageRules rules = MarriageRules::defaults();
    Scene s(neutral());
    const auto eval = s.evaluate(rules, kStandardDowry);

    // The groom's house RECEIVES the dowry: +10 points (270 roubles x 4 per 100 roubles).
    const auto& groom_side = eval.score(MatchSide::Groom);
    CHECK(groom_side.terms[idx(ScoreTerm::HonorGap)] == 0);
    CHECK(groom_side.terms[idx(ScoreTerm::RankGap)] == 0);
    CHECK(groom_side.terms[idx(ScoreTerm::Dowry)] == 10);
    CHECK(groom_side.terms[idx(ScoreTerm::Relation)] == 0);
    CHECK(groom_side.terms[idx(ScoreTerm::OpinionOfProposer)] == 0);
    CHECK(groom_side.terms[idx(ScoreTerm::MemberQuality)] == 0);
    CHECK(groom_side.terms[idx(ScoreTerm::AgeGap)] == 0);  // 5 years: inside the free ten
    CHECK(groom_side.terms[idx(ScoreTerm::External)] == 0);
    CHECK(groom_side.score == 10);
    CHECK(groom_side.accepted);
    CHECK(groom_side.refusal_code == 0);

    // The bride's house PAYS it: -10, a refusal whose reason is the dowry.
    const auto& bride_side = eval.score(MatchSide::Bride);
    CHECK(bride_side.terms[idx(ScoreTerm::Dowry)] == -10);
    CHECK(bride_side.score == -10);
    CHECK_FALSE(bride_side.accepted);
    CHECK(bride_side.refusal_code == marriage_term::kDowry);
}

TEST_CASE("accept_score: every term computes as the specification says") {
    const MarriageRules rules = MarriageRules::defaults();
    SceneParams p;
    p.groom_honor = 40;   // rank 1
    p.bride_honor = 340;  // rank 2
    p.groom_age = 40;     // gap 22 years: 12 above the free ten
    p.bride_age = 18;
    p.groom_health = 90;     // +40 x 0.2 = 8
    p.bride_health = 70;     // +20 x 0.2 = 4
    p.bride_fertility = 80;  // +30 x 0.2 = 6
    p.groom_skills = 10;     // sum 50: +10 x 0.15 = 1 (truncated from 1.5)
    p.bride_skills = 6;      // sum 30: -10 x 0.15 = -1 (truncated from -1.5)
    p.opinion = 30;          // x 0.5 = 15
    Scene s(p);
    MatchContext context;
    context.initiator = s.third_house;
    context.relation = -45;  // x 0.5 = -22 (truncated from -22.5)
    context.political_gain = {7, -4};
    context.faith_penalty = {3, 9};
    const auto eval = s.evaluate(rules, 20'000, context);

    // Seen from the groom's house (honor 40, rank 1) about the bride (honor 340, rank 2).
    const auto& g = eval.score(MatchSide::Groom);
    CHECK(g.terms[idx(ScoreTerm::HonorGap)] == 45);  // +300 x 0.15
    CHECK(g.terms[idx(ScoreTerm::RankGap)] == 0);    // the other house stands higher
    CHECK(g.terms[idx(ScoreTerm::Dowry)] == 8);      // +200 roubles x 4 per 100
    CHECK(g.terms[idx(ScoreTerm::Relation)] == -22);
    CHECK(g.terms[idx(ScoreTerm::OpinionOfProposer)] == 15);
    CHECK(g.health_part == 4);     // the bride: 70
    CHECK(g.fertility_part == 6);  // the bride: 80
    CHECK(g.skills_part == -1);    // the bride: 30
    CHECK(g.terms[idx(ScoreTerm::MemberQuality)] == 9);
    CHECK(g.terms[idx(ScoreTerm::AgeGap)] == -96);  // 12 years above ten, 8 each
    CHECK(g.political_gain_part == 7);
    CHECK(g.faith_part == -3);
    CHECK(g.terms[idx(ScoreTerm::External)] == 4);
    CHECK(g.score == 45 + 0 + 8 - 22 + 15 + 9 - 96 + 4);
    CHECK_FALSE(g.accepted);
    CHECK(g.refusal_code == marriage_term::kAgeGap);

    // Seen from the bride's house (honor 340, rank 2) about the groom (honor 40, rank 1).
    const auto& b = eval.score(MatchSide::Bride);
    CHECK(b.terms[idx(ScoreTerm::HonorGap)] == -45);
    CHECK(b.terms[idx(ScoreTerm::RankGap)] == -40);  // one rank above the groom's house
    CHECK(b.terms[idx(ScoreTerm::Dowry)] == -8);
    CHECK(b.terms[idx(ScoreTerm::Relation)] == -22);
    CHECK(b.terms[idx(ScoreTerm::OpinionOfProposer)] == 15);
    CHECK(b.health_part == 8);     // the groom: 90
    CHECK(b.fertility_part == 0);  // fertility counts for the bride only
    CHECK(b.skills_part == 1);     // the groom: 50
    CHECK(b.terms[idx(ScoreTerm::MemberQuality)] == 9);
    CHECK(b.terms[idx(ScoreTerm::AgeGap)] == -96);
    CHECK(b.political_gain_part == -4);
    CHECK(b.faith_part == -9);
    CHECK(b.terms[idx(ScoreTerm::External)] == -13);
    CHECK(b.score == -45 - 40 - 8 - 22 + 15 + 9 - 96 - 13);
}

TEST_CASE("accept_score: the opinion is that of the answering house's head, and a proposer asks nobody") {
    const MarriageRules rules = MarriageRules::defaults();
    SceneParams p = neutral();
    p.opinion = -40;
    Scene s(p);

    MatchContext by_third;
    by_third.initiator = s.third_house;
    auto eval = s.evaluate(rules, kStandardDowry, by_third);
    CHECK(eval.score(MatchSide::Groom).terms[idx(ScoreTerm::OpinionOfProposer)] == -20);
    CHECK(eval.score(MatchSide::Bride).terms[idx(ScoreTerm::OpinionOfProposer)] == -20);

    // The groom's house sends the envoy: its own opinion of itself does not count; the bride's does
    // not exist (nobody holds an opinion of the groom's head).
    MatchContext by_groom;
    by_groom.initiator = s.groom_house;
    eval = s.evaluate(rules, kStandardDowry, by_groom);
    CHECK(eval.score(MatchSide::Groom).terms[idx(ScoreTerm::OpinionOfProposer)] == 0);
    CHECK(eval.score(MatchSide::Bride).terms[idx(ScoreTerm::OpinionOfProposer)] == 0);

    // No proposer: no opinion. The opinion of a dead head is not read.
    eval = s.evaluate(rules, kStandardDowry);
    CHECK(eval.score(MatchSide::Groom).terms[idx(ScoreTerm::OpinionOfProposer)] == 0);
    REQUIRE(s.world.die(s.bride_head, CauseOfDeath::Illness));
    eval = s.evaluate(rules, kStandardDowry, by_third);
    CHECK(eval.score(MatchSide::Groom).terms[idx(ScoreTerm::OpinionOfProposer)] == -20);
    CHECK(eval.score(MatchSide::Bride).terms[idx(ScoreTerm::OpinionOfProposer)] == 0);
}

TEST_CASE("accept_score: a negative or absurd dowry cannot be an input") {
    const MarriageRules rules = MarriageRules::defaults();
    Scene s(neutral());
    const auto negative = s.evaluate(rules, -500'000);
    CHECK(negative.dowry_kop == 0);
    CHECK(negative.score(MatchSide::Groom).terms[idx(ScoreTerm::Dowry)] == 0);
    const auto huge = s.evaluate(rules, std::numeric_limits<Kopecks>::max());
    CHECK(huge.score(MatchSide::Groom).terms[idx(ScoreTerm::Dowry)] == rules.score.dowry_points_cap);
    CHECK(huge.score(MatchSide::Bride).terms[idx(ScoreTerm::Dowry)] == -rules.score.dowry_points_cap);
}

TEST_CASE("evaluate_match: people who are dead, unknown or without a house have no evaluation") {
    const MarriageRules rules = MarriageRules::defaults();
    Scene s(neutral());
    CHECK_FALSE(evaluate_match(s.world, rules, CharacterId{}, s.bride, 0).has_value());
    CHECK_FALSE(evaluate_match(s.world, rules, s.groom, CharacterId(9999), 0).has_value());
    CHECK(evaluate_match(s.world, rules, s.groom, s.bride, 0).has_value());
    REQUIRE(s.world.die(s.groom, CauseOfDeath::Illness));
    CHECK_FALSE(evaluate_match(s.world, rules, s.groom, s.bride, 0).has_value());
}

// ---- cases of the GDD table --------------------------------------------------------------------

TEST_CASE("accept_score: an old house refuses a poor one and money cannot buy it past the corridor") {
    const MarriageRules rules = MarriageRules::defaults();
    // The groom's house is great (honor 600, rank 3); the bride's is poor (honor 0, rank 1).
    SceneParams p = neutral();
    p.groom_honor = 600;
    p.bride_honor = 0;
    Scene s(p);

    const auto standard = s.evaluate(rules, standard_dowry(s.world, rules, s.bride_house));
    const auto& g = standard.score(MatchSide::Groom);
    CHECK(g.terms[idx(ScoreTerm::HonorGap)] == -90);
    CHECK(g.terms[idx(ScoreTerm::RankGap)] == -80);
    CHECK_FALSE(g.accepted);
    CHECK(g.refusal_code == marriage_term::kHonorGap);

    // The largest dowry there is gives at most the corridor of the term.
    const auto richest = s.evaluate(rules, rules.money.dowry_max_kop);
    CHECK(richest.score(MatchSide::Groom).terms[idx(ScoreTerm::Dowry)] == rules.score.dowry_points_cap);
    CHECK_FALSE(richest.score(MatchSide::Groom).accepted);
    const auto absurd = s.evaluate(rules, rules.money.dowry_max_kop * 1000);
    CHECK(absurd.score(MatchSide::Groom).terms[idx(ScoreTerm::Dowry)] == rules.score.dowry_points_cap);
    CHECK_FALSE(absurd.score(MatchSide::Groom).accepted);

    // The poor bride's house, on the other side, is glad of the great match: honor and rank help or
    // do not hurt it, so only the dowry it pays weighs on the answer.
    const auto& b = standard.score(MatchSide::Bride);
    CHECK(b.terms[idx(ScoreTerm::HonorGap)] == 90);
    CHECK(b.terms[idx(ScoreTerm::RankGap)] == 0);
    CHECK(b.accepted);
}

TEST_CASE("accept_score: the dowry compensates a gap of honor up to the point where the score reaches zero") {
    const MarriageRules rules = MarriageRules::defaults();
    // The groom's house stands 90 points above the bride's, in the same rank: -13.5 truncates to -13.
    SceneParams p = neutral();
    p.groom_honor = 90;
    p.bride_honor = 0;
    Scene s(p);

    CHECK(s.evaluate(rules, 0).score(MatchSide::Groom).score == -13);
    // 100 roubles = +4, 270 roubles = +10: not enough.
    const auto standard = s.evaluate(rules, kStandardDowry);
    CHECK(standard.score(MatchSide::Groom).score == -3);
    CHECK_FALSE(standard.score(MatchSide::Groom).accepted);
    // 325 roubles = +13: exactly enough (the threshold is "at least").
    const auto enough = s.evaluate(rules, 32'500);
    CHECK(enough.score(MatchSide::Groom).score == 0);
    CHECK(enough.score(MatchSide::Groom).accepted);
    CHECK(enough.score(MatchSide::Groom).refusal_code == 0);
    // And a bigger dowry cannot make it worse.
    CHECK(s.evaluate(rules, 100'000).score(MatchSide::Groom).accepted);
}

TEST_CASE("accept_score: houses of the same honor and rank, the standard dowry (the border case)") {
    const MarriageRules rules = MarriageRules::defaults();
    // A healthy, fertile bride and a groom of ordinary qualities, as in the table of the model.
    SceneParams p = neutral();
    p.bride_health = 75;
    p.bride_fertility = 75;
    p.groom_health = 60;
    p.groom_skills = 10;  // sum 50
    Scene s(p);
    const auto eval = s.evaluate(rules, kStandardDowry);
    // The groom's house: +10 dowry, +5 +5 for the bride; the answer is clearly yes.
    CHECK(eval.score(MatchSide::Groom).score == 20);
    CHECK(eval.score(MatchSide::Groom).accepted);
    // The bride's house: -10 dowry, +2 +1 for the groom; the answer is no, by the dowry.
    CHECK(eval.score(MatchSide::Bride).score == -7);
    CHECK(eval.score(MatchSide::Bride).refusal_code == marriage_term::kDowry);
}

// ---- sum, explanation, determinism -------------------------------------------------------------

TEST_CASE("accept_score: the terms add up exactly and the Breakdown tells the same story") {
    const MarriageRules rules = MarriageRules::defaults();
    SceneParams p;
    p.groom_honor = 700;
    p.bride_honor = 120;
    p.groom_age = 60;
    p.bride_age = 16;
    p.groom_health = 20;
    p.bride_health = 95;
    p.bride_fertility = 100;
    p.groom_skills = 15;
    p.bride_skills = 2;
    p.opinion = -33;
    Scene s(p);
    MatchContext context;
    context.initiator = s.third_house;
    context.relation = 61;
    context.political_gain = {12, 3};
    context.faith_penalty = {50, 0};
    const auto eval = s.evaluate(rules, 61'234, context);

    for (const MatchSide side : {MatchSide::Groom, MatchSide::Bride}) {
        const AcceptScore& score = eval.score(side);
        CHECK(score.score == score.sum_of_terms());
        CHECK(score.threshold == rules.score.accept_threshold);
        CHECK(score.accepted == (score.score >= score.threshold));
        CHECK(score.terms[idx(ScoreTerm::MemberQuality)] ==
              score.health_part + score.fertility_part + score.skills_part);
        CHECK(score.terms[idx(ScoreTerm::External)] == score.political_gain_part + score.faith_part);

        Breakdown explain;
        const AcceptScore& same = accept_score(eval, side, &explain);
        CHECK(&same == &eval.score(side));
        CHECK_FALSE(explain.truncated());
        CHECK(explain.is_consistent());
        CHECK(explain.root().code == marriage_term::kAcceptScore);
        CHECK(explain.root().unit == pat::sim::economy::Unit::Points);
        CHECK(explain.total() == score.score);
        CHECK(explain.child_count(pat::sim::economy::kRootNode) == kScoreTermCount);
        for (std::size_t i = 0; i < kScoreTermCount; ++i) {
            const auto node = explain.find(score_term_code(static_cast<ScoreTerm>(i)));
            REQUIRE(node != pat::sim::economy::kNoNode);
            CHECK(explain.node(node).value == score.terms[i]);
        }
        CHECK(explain.child_count(explain.find(marriage_term::kMemberQuality)) == 3);
        CHECK(explain.child_count(explain.find(marriage_term::kExternal)) == 2);
        CHECK(explain.node(explain.find(marriage_term::kFaithPenalty)).value == score.faith_part);
        CHECK(explain.size() <= pat::sim::economy::kBreakdownCapacity);
        CHECK(explain.size() == 1 + kScoreTermCount + 3 + 2);
    }
}

TEST_CASE("accept_score: the refusal reason is the weakest term, the first one on a tie") {
    const MarriageRules rules = MarriageRules::defaults();
    SceneParams p = neutral();
    p.groom_age = 60;  // a gap of 40 years: 30 above the free ten (the cap), -240
    p.bride_age = 20;
    Scene s(p);
    const auto eval = s.evaluate(rules, kStandardDowry);
    CHECK(eval.score(MatchSide::Groom).score == -230);
    CHECK_FALSE(eval.score(MatchSide::Groom).accepted);
    CHECK(eval.score(MatchSide::Groom).refusal_code == marriage_term::kAgeGap);

    // Every term is 0 and the threshold is above zero: the first term is named.
    auto strict = rules;
    strict.score.accept_threshold = 5;
    Scene calm(neutral());
    const auto tie = *evaluate_match(calm.world, strict, calm.groom, calm.bride, 0);
    CHECK(tie.score(MatchSide::Groom).score == 0);
    CHECK_FALSE(tie.score(MatchSide::Groom).accepted);
    CHECK(tie.score(MatchSide::Groom).refusal_code == marriage_term::kHonorGap);
    CHECK(tie.score(MatchSide::Groom).threshold == 5);

    // A score exactly at the threshold agrees.
    strict.score.accept_threshold = 0;
    CHECK(evaluate_match(calm.world, strict, calm.groom, calm.bride, 0)->score(MatchSide::Groom).accepted);
}

TEST_CASE("accept_score: evaluation is deterministic, draws nothing and leaves the world untouched") {
    const MarriageRules rules = MarriageRules::defaults();
    SceneParams p;
    p.groom_honor = 210;
    p.opinion = 12;
    Scene s(p);
    MatchContext context;
    context.initiator = s.third_house;
    context.relation = 8;

    const WorldHash before = s.world.compute_hash();
    const auto a = s.evaluate(rules, 31'000, context);
    const auto b = s.evaluate(rules, 31'000, context);
    const World copy = s.world;
    const auto c = *evaluate_match(copy, rules, s.groom, s.bride, 31'000, context);
    CHECK(a == b);
    CHECK(a == c);
    CHECK(s.world.compute_hash() == before);  // no state, no random stream touched
    CHECK(s.world.compute_hash().rng == before.rng);
}

// ---- monotonicity and bounds -------------------------------------------------------------------

TEST_CASE("accept_score: monotone in every input") {
    const MarriageRules rules = MarriageRules::defaults();
    const auto score_of =
        [&](const SceneParams& p, Kopecks dowry, MatchSide side, MatchContext context = {}) {
            Scene s(p);
            context.initiator = s.third_house;
            return s.evaluate(rules, dowry, context).score(side).score;
        };

    // More dowry: the house that receives it never scores less, the house that pays it never more.
    std::int32_t last_groom = std::numeric_limits<std::int32_t>::min();
    std::int32_t last_bride = std::numeric_limits<std::int32_t>::max();
    for (Kopecks dowry = 0; dowry <= 150'000; dowry += 2'500) {
        const auto g = score_of(neutral(), dowry, MatchSide::Groom);
        const auto b = score_of(neutral(), dowry, MatchSide::Bride);
        CHECK(g >= last_groom);
        CHECK(b <= last_bride);
        last_groom = g;
        last_bride = b;
    }

    // A higher honor of the OTHER house helps, a higher honor of the own house does not.
    for (const MatchSide side : {MatchSide::Groom, MatchSide::Bride}) {
        std::int32_t last_other = std::numeric_limits<std::int32_t>::min();
        std::int32_t last_own = std::numeric_limits<std::int32_t>::max();
        for (std::int32_t honor = -900; honor <= 1500; honor += 150) {
            SceneParams other = neutral();
            SceneParams own = neutral();
            (side == MatchSide::Groom ? other.bride_honor : other.groom_honor) = honor;
            (side == MatchSide::Groom ? own.groom_honor : own.bride_honor) = honor;
            const auto value_other = score_of(other, kStandardDowry, side);
            const auto value_own = score_of(own, kStandardDowry, side);
            CHECK(value_other >= last_other);
            CHECK(value_own <= last_own);
            last_other = value_other;
            last_own = value_own;
        }
    }

    // A bigger age gap never helps (the younger bride is the same, the groom gets older).
    std::int32_t last_gap = std::numeric_limits<std::int32_t>::max();
    for (int age = 20; age <= 90; age += 4) {
        SceneParams p = neutral();
        p.groom_age = age;
        const auto value = score_of(p, kStandardDowry, MatchSide::Groom);
        CHECK(value <= last_gap);
        last_gap = value;
    }

    // Better qualities of the other person never hurt: health, fertility (groom's side), skills.
    std::int32_t last_health = std::numeric_limits<std::int32_t>::min();
    std::int32_t last_fertility = std::numeric_limits<std::int32_t>::min();
    std::int32_t last_skills = std::numeric_limits<std::int32_t>::min();
    for (int q = 0; q <= 100; q += 10) {
        SceneParams h = neutral();
        h.bride_health = q;
        SceneParams f = neutral();
        f.bride_fertility = q;
        SceneParams k = neutral();
        k.bride_skills = q / 5;
        const auto vh = score_of(h, kStandardDowry, MatchSide::Groom);
        const auto vf = score_of(f, kStandardDowry, MatchSide::Groom);
        const auto vk = score_of(k, kStandardDowry, MatchSide::Groom);
        CHECK(vh >= last_health);
        CHECK(vf >= last_fertility);
        CHECK(vk >= last_skills);
        last_health = vh;
        last_fertility = vf;
        last_skills = vk;
    }
    // The fertility of the groom is nobody's concern.
    {
        Scene fertile(neutral());
        Scene barren(neutral());
        fertile.world.set_character_fertility(fertile.groom, points(100));
        barren.world.set_character_fertility(barren.groom, points(0));
        const auto f = fertile.evaluate(rules, kStandardDowry);
        const auto b = barren.evaluate(rules, kStandardDowry);
        CHECK(f.score(MatchSide::Bride).score == b.score(MatchSide::Bride).score);
        CHECK(f.score(MatchSide::Groom).score == b.score(MatchSide::Groom).score);
    }

    // A better relation and a better opinion of the proposer help; so does a political gain.
    std::int32_t last_relation = std::numeric_limits<std::int32_t>::min();
    std::int32_t last_opinion = std::numeric_limits<std::int32_t>::min();
    std::int32_t last_gain = std::numeric_limits<std::int32_t>::min();
    for (int v = -100; v <= 100; v += 10) {
        MatchContext relation;
        relation.relation = v;
        SceneParams o = neutral();
        o.opinion = v;
        MatchContext gain;
        gain.political_gain = {v, v};
        const auto vr = score_of(neutral(), kStandardDowry, MatchSide::Groom, relation);
        const auto vo = score_of(o, kStandardDowry, MatchSide::Groom);
        const auto vg = score_of(neutral(), kStandardDowry, MatchSide::Groom, gain);
        CHECK(vr >= last_relation);
        CHECK(vo >= last_opinion);
        CHECK(vg >= last_gain);
        last_relation = vr;
        last_opinion = vo;
        last_gain = vg;
    }
}

namespace {

struct WeightRow {
    std::int32_t MarriageScoreRules::* field;
    std::int32_t (*value)(const AcceptScore&);
    const char* name;
};

std::int32_t term_value(const AcceptScore& s, ScoreTerm term) {
    return s.terms[idx(term)];
}

}  // namespace

TEST_CASE("accept_score: a larger weight amplifies its term and never reverses it") {
    const std::array rows{
        WeightRow{&MarriageScoreRules::honor_weight_permille,
                  [](const AcceptScore& s) { return term_value(s, ScoreTerm::HonorGap); },
                  "honor"},
        WeightRow{&MarriageScoreRules::rank_step_penalty,
                  [](const AcceptScore& s) { return term_value(s, ScoreTerm::RankGap); },
                  "rank"},
        WeightRow{&MarriageScoreRules::dowry_points_per_100rub,
                  [](const AcceptScore& s) { return term_value(s, ScoreTerm::Dowry); },
                  "dowry"},
        WeightRow{&MarriageScoreRules::relation_weight_permille,
                  [](const AcceptScore& s) { return term_value(s, ScoreTerm::Relation); },
                  "relation"},
        WeightRow{&MarriageScoreRules::opinion_weight_permille,
                  [](const AcceptScore& s) { return term_value(s, ScoreTerm::OpinionOfProposer); },
                  "opinion"},
        WeightRow{&MarriageScoreRules::health_weight_permille,
                  [](const AcceptScore& s) { return s.health_part; },
                  "health"},
        WeightRow{&MarriageScoreRules::fertility_weight_permille,
                  [](const AcceptScore& s) { return s.fertility_part; },
                  "fertility"},
        WeightRow{&MarriageScoreRules::skills_weight_permille,
                  [](const AcceptScore& s) { return s.skills_part; },
                  "skills"},
        WeightRow{&MarriageScoreRules::age_gap_penalty,
                  [](const AcceptScore& s) { return term_value(s, ScoreTerm::AgeGap); },
                  "age gap"},
    };
    const MarriageRules base = MarriageRules::defaults();

    // A grid of scenes covering plus and minus for every term.
    for (const std::int32_t groom_honor : {-300, 0, 90, 250, 800}) {
        for (const std::int32_t bride_honor : {-300, 0, 120, 700}) {
            for (const int groom_age : {18, 30, 55}) {
                for (const int health : {10, 50, 90}) {
                    SceneParams p = neutral();
                    p.groom_honor = groom_honor;
                    p.bride_honor = bride_honor;
                    p.groom_age = groom_age;
                    p.bride_health = health;
                    p.bride_fertility = health;
                    p.groom_health = health;
                    p.bride_skills = health / 5;
                    p.groom_skills = health / 5;
                    p.opinion = health - 50;
                    Scene s(p);
                    MatchContext context;
                    context.initiator = s.third_house;
                    context.relation = health - 50;
                    for (const auto& row : rows) {
                        auto stronger = base;
                        stronger.score.*row.field += 250;
                        const auto weak_eval =
                            *evaluate_match(s.world, base, s.groom, s.bride, kStandardDowry, context);
                        const auto strong_eval =
                            *evaluate_match(s.world, stronger, s.groom, s.bride, kStandardDowry, context);
                        for (const MatchSide side : {MatchSide::Groom, MatchSide::Bride}) {
                            // Each term separately, parts included.
                            const auto& weak = weak_eval.score(side);
                            const auto& strong = strong_eval.score(side);
                            CAPTURE(row.name);
                            const auto before = row.value(weak);
                            const auto after = row.value(strong);
                            CHECK(std::abs(after) >= std::abs(before));
                            CHECK(static_cast<std::int64_t>(after) * before >= 0);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("accept_score: scale bounds, corridors and the whole range of the score") {
    const MarriageRules rules = MarriageRules::defaults();
    const TermBounds total = score_bounds(rules);
    CHECK(total.min < 0);
    CHECK(total.max > 0);

    // The documented bounds of the default rules.
    CHECK(term_bounds(rules, ScoreTerm::HonorGap).min == -90);
    CHECK(term_bounds(rules, ScoreTerm::HonorGap).max == 90);
    CHECK(term_bounds(rules, ScoreTerm::RankGap).min == -160);
    CHECK(term_bounds(rules, ScoreTerm::RankGap).max == 0);
    CHECK(term_bounds(rules, ScoreTerm::Dowry).min == -30);
    CHECK(term_bounds(rules, ScoreTerm::Dowry).max == 30);
    CHECK(term_bounds(rules, ScoreTerm::Relation).max == 50);
    CHECK(term_bounds(rules, ScoreTerm::OpinionOfProposer).min == -50);
    CHECK(term_bounds(rules, ScoreTerm::MemberQuality).min == -26);  // -10 -10 -6
    CHECK(term_bounds(rules, ScoreTerm::MemberQuality).max == 29);   // 10 10 9
    CHECK(term_bounds(rules, ScoreTerm::AgeGap).min == -240);
    CHECK(term_bounds(rules, ScoreTerm::External).min == -130);
    CHECK(term_bounds(rules, ScoreTerm::External).max == 30);

    // The extremes of the inputs reach the bounds and never leave them.
    for (const std::int32_t groom_honor : {-1000, 0, 400, 10000}) {
        for (const std::int32_t bride_honor : {-1000, 0, 400, 10000}) {
            for (const int groom_age : {0, 16, 80, 100}) {
                for (const int quality : {0, 100}) {
                    SceneParams p;
                    p.groom_honor = groom_honor;
                    p.bride_honor = bride_honor;
                    p.groom_age = groom_age;
                    p.bride_age = 15;
                    p.groom_health = quality;
                    p.bride_health = quality;
                    p.bride_fertility = quality;
                    p.groom_skills = quality / 5;
                    p.bride_skills = quality / 5;
                    p.opinion = quality == 0 ? -100 : 100;
                    Scene s(p);
                    for (const std::int32_t extreme : {-100000, 100000}) {
                        MatchContext context;
                        context.initiator = s.third_house;
                        context.relation = extreme;
                        context.political_gain = {extreme, -extreme};
                        context.faith_penalty = {extreme, -extreme};
                        for (const Kopecks dowry : {Kopecks{0}, Kopecks{500'000'000}}) {
                            const auto eval = s.evaluate(rules, dowry, context);
                            for (const MatchSide side : {MatchSide::Groom, MatchSide::Bride}) {
                                const auto& score = eval.score(side);
                                for (std::size_t i = 0; i < kScoreTermCount; ++i) {
                                    const auto bounds = term_bounds(rules, static_cast<ScoreTerm>(i));
                                    CHECK(score.terms[i] >= bounds.min);
                                    CHECK(score.terms[i] <= bounds.max);
                                }
                                CHECK(score.score >= total.min);
                                CHECK(score.score <= total.max);
                            }
                        }
                    }
                }
            }
        }
    }

    // The ends of the scales are reached.
    {
        SceneParams extreme = neutral();
        extreme.groom_honor = -1000;
        extreme.bride_honor = 10000;
        extreme.groom_age = 80;
        extreme.bride_age = 15;
        Scene s_extreme(extreme);
        const auto e = s_extreme.evaluate(rules, 0);
        CHECK(e.score(MatchSide::Groom).terms[idx(ScoreTerm::HonorGap)] == 90);
        CHECK(e.score(MatchSide::Bride).terms[idx(ScoreTerm::HonorGap)] == -90);
        CHECK(e.score(MatchSide::Bride).terms[idx(ScoreTerm::RankGap)] == -160);
        CHECK(e.score(MatchSide::Groom).terms[idx(ScoreTerm::AgeGap)] == -240);
    }

    // The corridors really clamp: a huge relation, gain or penalty equals the limit.
    Scene s(neutral());
    MatchContext wild;
    wild.relation = 99999;
    wild.political_gain = {99999, 99999};
    wild.faith_penalty = {99999, 99999};
    const auto eval = s.evaluate(rules, 0, wild);
    const auto& g = eval.score(MatchSide::Groom);
    CHECK(g.terms[idx(ScoreTerm::Relation)] == 50);
    CHECK(g.political_gain_part == rules.score.political_gain_cap);
    CHECK(g.faith_part == -rules.score.faith_penalty_cap);
    wild.relation = -99999;
    wild.faith_penalty = {-50, -50};  // a negative penalty is not a bonus
    wild.political_gain = {-99999, -99999};
    const auto low = s.evaluate(rules, 0, wild).score(MatchSide::Groom);
    CHECK(low.terms[idx(ScoreTerm::Relation)] == -50);
    CHECK(low.political_gain_part == -rules.score.political_gain_cap);
    CHECK(low.faith_part == 0);
}

// ---- forecast and keys -------------------------------------------------------------------------

TEST_CASE("forecast: only the public terms count, with a margin around the threshold") {
    const MarriageRules rules = MarriageRules::defaults();
    CHECK_FALSE(is_public_term(ScoreTerm::OpinionOfProposer));
    CHECK_FALSE(is_public_term(ScoreTerm::MemberQuality));
    CHECK_FALSE(is_public_term(ScoreTerm::External));
    CHECK(is_public_term(ScoreTerm::HonorGap));
    CHECK(is_public_term(ScoreTerm::RankGap));
    CHECK(is_public_term(ScoreTerm::Dowry));
    CHECK(is_public_term(ScoreTerm::Relation));
    CHECK(is_public_term(ScoreTerm::AgeGap));

    // Public terms: the dowry +10 on the groom's side; margin 15 gives "unclear" in [-15, 15).
    Scene s(neutral());
    auto eval = s.evaluate(rules, kStandardDowry);
    CHECK(forecast_of(rules, eval.score(MatchSide::Groom)) == Forecast::Unclear);
    CHECK(forecast_of(rules, eval.score(MatchSide::Bride)) == Forecast::Unclear);

    // The hidden terms never move the forecast: great health and skills, a friendly head, a gain.
    SceneParams hidden = neutral();
    hidden.bride_health = 100;
    hidden.bride_fertility = 100;
    hidden.bride_skills = 20;
    hidden.opinion = 100;
    Scene friendly(hidden);
    MatchContext context;
    context.initiator = friendly.third_house;
    context.political_gain = {30, 30};
    const auto friendly_eval = friendly.evaluate(rules, kStandardDowry, context);
    CHECK(friendly_eval.score(MatchSide::Groom).public_sum() == eval.score(MatchSide::Groom).public_sum());
    CHECK(forecast_of(rules, friendly_eval.score(MatchSide::Groom)) == Forecast::Unclear);
    CHECK(friendly_eval.score(MatchSide::Groom).score > eval.score(MatchSide::Groom).score);

    // A great relation and a rich dowry make it likely; a gap of honor and rank, unlikely.
    context = {};
    context.relation = 100;
    context.initiator = s.third_house;
    CHECK(forecast_of(rules, s.evaluate(rules, kStandardDowry, context).score(MatchSide::Groom)) ==
          Forecast::Likely);
    SceneParams proud = neutral();
    proud.groom_honor = 600;
    Scene scene_proud(proud);
    CHECK(forecast_of(rules, scene_proud.evaluate(rules, kStandardDowry).score(MatchSide::Groom)) ==
          Forecast::Unlikely);
}

TEST_CASE("marriage terms: every issued code has a unique localisation key") {
    constexpr std::array codes{
        marriage_term::kAcceptScore,     marriage_term::kHonorGap,     marriage_term::kRankGap,
        marriage_term::kDowry,           marriage_term::kRelation,     marriage_term::kOpinionOfProposer,
        marriage_term::kMemberQuality,   marriage_term::kAgeGap,       marriage_term::kExternal,
        marriage_term::kPoliticalGain,   marriage_term::kHealth,       marriage_term::kFertility,
        marriage_term::kSkills,          marriage_term::kFaithPenalty, marriage_term::kEnvoyTotal,
        marriage_term::kEnvoyRoad,       marriage_term::kEnvoyGifts,   marriage_term::kEnvoyDispensation,
        marriage_term::kDowryTotal,      marriage_term::kDowryBase,    marriage_term::kDowryHonor,
        marriage_term::kDowryWidowClaim, marriage_term::kDowryLimit,
    };
    for (std::size_t i = 0; i < codes.size(); ++i) {
        CAPTURE(codes[i]);
        const std::string_view key = marriage_term_key(codes[i]);
        CHECK_FALSE(key.empty());
        CHECK(key.starts_with("marriage."));
        for (std::size_t j = 0; j < i; ++j) {
            CHECK(marriage_term_key(codes[j]) != key);
        }
    }
    CHECK(marriage_term_key(0).empty());
    CHECK(marriage_term_key(10099).empty());

    // The codes of the eight terms follow the order of the enumeration.
    CHECK(score_term_code(ScoreTerm::HonorGap) == 10001);
    CHECK(score_term_code(ScoreTerm::External) == 10008);
}
