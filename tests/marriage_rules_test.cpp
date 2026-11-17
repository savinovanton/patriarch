#include "pat/sim/family/tables.hpp"
#include "pat/sim/marriage_rules.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace {

using namespace pat::sim;

template <typename Group>
struct FieldBounds {
    const char* field;
    std::int32_t Group::* member;
    std::int32_t min;
    std::int32_t max;
};

template <typename Group, std::size_t N>
void check_bounds(Group MarriageRules::* group, const std::array<FieldBounds<Group>, N>& bounds) {
    for (const auto& bound : bounds) {
        CAPTURE(bound.field);
        for (const auto value : {bound.min, bound.max}) {
            auto rules = MarriageRules::defaults();
            (rules.*group).*bound.member = value;
            CHECK(validate(rules).has_value());
        }
        for (const auto value : {bound.min - 1, bound.max + 1}) {
            auto rules = MarriageRules::defaults();
            (rules.*group).*bound.member = value;
            const auto error = validate(rules);
            REQUIRE_FALSE(error.has_value());
            CHECK(error.error().field == bound.field);
        }
    }
}

}  // namespace

TEST_CASE("marriage rules: negotiation bounds preserve usable dates, capacity and refunds") {
    using G = MarriageNegotiationRules;
    check_bounds(
        &MarriageRules::negotiation,
        std::array{
            FieldBounds<G>{"negotiation.answer_days", &G::answer_days, 1, 36'500},
            FieldBounds<G>{"negotiation.max_parallel_negotiations",
                           &G::max_parallel_negotiations,
                           1,
                           static_cast<std::int32_t>(family::kMaxNegotiations)},
            FieldBounds<G>{"negotiation.refusal_cooldown_days", &G::refusal_cooldown_days, 0, 36'500},
            FieldBounds<G>{"negotiation.withdraw_refund_permille", &G::withdraw_refund_permille, 0, 1000},
            FieldBounds<G>{"negotiation.wedding_delay_days", &G::wedding_delay_days, 0, 36'500},
            FieldBounds<G>{"negotiation.wedding_grace_days", &G::wedding_grace_days, 1, 36'500},
        });
}

TEST_CASE("marriage rules: consequences distinguish penalties, refunds and widow claims") {
    using G = MarriageConsequenceRules;
    check_bounds(
        &MarriageRules::consequences,
        std::array{
            FieldBounds<G>{"consequences.dispensation_honor", &G::dispensation_honor, 0, 10'000},
            FieldBounds<G>{"consequences.marriage_accepted_opinion", &G::marriage_accepted_opinion, 0, 100},
            FieldBounds<G>{"consequences.marriage_refused_opinion", &G::marriage_refused_opinion, 0, 100},
            FieldBounds<G>{"consequences.betrothal_broken_honor", &G::betrothal_broken_honor, 0, 10'000},
            FieldBounds<G>{"consequences.betrothal_broken_opinion", &G::betrothal_broken_opinion, 0, 100},
            FieldBounds<G>{"consequences.tonsure_honor", &G::tonsure_honor, 0, 10'000},
            FieldBounds<G>{"consequences.tonsure_unity", &G::tonsure_unity, 0, 100},
            FieldBounds<G>{"consequences.tonsure_opinion", &G::tonsure_opinion, 0, 100},
            FieldBounds<G>{"consequences.tonsure_refund_permille", &G::tonsure_refund_permille, 0, 1000},
            FieldBounds<G>{"consequences.third_marriage_honor", &G::third_marriage_honor, 0, 10'000},
            FieldBounds<G>{"consequences.wedding_rank_honor", &G::wedding_rank_honor, 0, 10'000},
            FieldBounds<G>{"consequences.married_into_opinion", &G::married_into_opinion, 0, 100},
            FieldBounds<G>{"consequences.widow_claim_permille", &G::widow_claim_permille, 0, 10'000},
        });
}

TEST_CASE("marriage rules: matchmaker policy accepts either flag and bounded signed scores") {
    using G = MarriageMatchmakerRules;
    check_bounds(&MarriageRules::matchmaker,
                 std::array{FieldBounds<G>{
                     "matchmaker.matchmaker_min_value", &G::matchmaker_min_value, -100'000, 100'000}});
    auto rules = MarriageRules::defaults();
    rules.matchmaker.ai_may_propose_to_player = true;
    rules.consequences.bride_joins_groom_house = false;
    CHECK(validate(rules).has_value());
}

TEST_CASE("marriage rules: consistency errors identify the exact offending field") {
    auto rules = MarriageRules::defaults();
    rules.ranks.min_honor[2] = rules.ranks.min_honor[1];
    auto error = validate(rules);
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error().field == "ranks.min_honor[2]");

    rules = MarriageRules::defaults();
    rules.money.dowry_min_kop = rules.money.dowry_max_kop + 1;
    error = validate(rules);
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error().field == "money.dowry_min_kop");

    rules = MarriageRules::defaults();
    rules.eligibility.min_betrothal_age = rules.eligibility.min_marriage_age_male + 1;
    error = validate(rules);
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error().field == "eligibility.min_betrothal_age");

    rules = MarriageRules::defaults();
    rules.eligibility.blood_absolute_degree = rules.eligibility.blood_forbidden_degree;
    --rules.eligibility.blood_forbidden_degree;
    error = validate(rules);
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error().field == "eligibility.blood_absolute_degree");
}

TEST_CASE("marriage rules: M1 pair restrictions cannot be disabled") {
    auto rules = MarriageRules::defaults();
    REQUIRE(validate(rules).has_value());
    std::string_view expected_field;
    SUBCASE("opposite sex is mandatory") {
        rules.eligibility.require_opposite_sex = false;
        expected_field = "eligibility.require_opposite_sex";
    }
    SUBCASE("different houses are mandatory") {
        rules.eligibility.forbid_same_house = false;
        expected_field = "eligibility.forbid_same_house";
    }
    const auto result = validate(rules);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().field == expected_field);
}
