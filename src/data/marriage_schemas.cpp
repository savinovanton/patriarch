#include "pat/data/marriage_schemas.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace pat::data {
namespace {

// Only representation limits belong here; sim::validate owns domain and cross-field rules.
Type i32() {
    return Type::integer(std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max());
}
Type i64() {
    return Type::integer(std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max());
}

ObjectSchema rank_schema() {
    ObjectSchema s("ranks");
    s.required("min_honor", Type::list(i32()))
        .describe("Пять строго возрастающих порогов чести брачных рангов.");
    return s;
}

ObjectSchema score_schema() {
    ObjectSchema s("score");
    for (const char* field : {"honor_weight_permille",
                              "relation_weight_permille",
                              "opinion_weight_permille",
                              "health_weight_permille",
                              "fertility_weight_permille",
                              "skills_weight_permille",
                              "rank_step_penalty",
                              "dowry_points_per_100rub",
                              "dowry_points_cap",
                              "age_gap_penalty",
                              "political_gain_cap",
                              "faith_penalty_cap"}) {
        s.required(field, i32());
    }
    s.required("honor_gap_limit", i32());
    for (const char* field : {"health_neutral", "fertility_neutral", "skills_neutral"}) {
        s.required(field, i32());
    }
    s.required("age_gap_free_years", i32())
        .required("age_gap_years_cap", i32())
        .required("accept_threshold", i32())
        .required("forecast_margin", i32());
    return s;
}

ObjectSchema money_schema() {
    ObjectSchema s("money");
    for (const char* field : {"dowry_base_kop", "dowry_min_kop", "dowry_max_kop"}) {
        s.required(field, i64());
    }
    s.required("dowry_per_honor_kop", i64());
    for (const char* field : {"envoy_road_kop", "envoy_gifts_kop", "dispensation_fee_kop"}) {
        s.required(field, i64());
    }
    return s;
}

ObjectSchema eligibility_schema() {
    ObjectSchema s("eligibility");
    for (const char* field : {"min_marriage_age_male", "min_marriage_age_female", "min_betrothal_age"}) {
        s.required(field, i32());
    }
    for (const char* field :
         {"blood_forbidden_degree", "blood_absolute_degree", "affinity_forbidden_degree"}) {
        s.required(field, i32());
    }
    s.required("mourning_days_female", i32())
        .required("mourning_days_male", i32())
        .required("require_opposite_sex", Type::boolean())
        .required("forbid_same_house", Type::boolean());
    return s;
}

ObjectSchema relation_schema() {
    ObjectSchema s("relation");
    s.required("marriage_tie_bonus", i32()).required("max_marriage_ties", i32());
    return s;
}

ObjectSchema negotiation_schema() {
    ObjectSchema s("negotiation");
    s.required("answer_days", i32())
        .required("max_parallel_negotiations", i32())
        .required("refusal_cooldown_days", i32())
        .required("withdraw_refund_permille", i32());
    return s;
}

ObjectSchema consequences_schema() {
    ObjectSchema s("consequences");
    for (const char* field : {"dispensation_honor",
                              "marriage_accepted_opinion",
                              "marriage_refused_opinion",
                              "betrothal_broken_honor",
                              "betrothal_broken_opinion",
                              "tonsure_honor",
                              "tonsure_unity",
                              "tonsure_opinion",
                              "third_marriage_honor",
                              "wedding_rank_honor",
                              "married_into_opinion"}) {
        s.required(field, i32());
    }
    s.required("tonsure_refund_permille", i32())
        .required("widow_claim_permille", i32())
        .required("bride_joins_groom_house", Type::boolean());
    return s;
}

ObjectSchema matchmaker_schema() {
    ObjectSchema s("matchmaker");
    s.required("matchmaker_min_value", i32()).required("ai_may_propose_to_player", Type::boolean());
    return s;
}

}  // namespace

void add_marriage_schemas(SchemaSet& set) {
    ObjectSchema s{std::string(kMarriageRulesKind)};
    s.describe_kind("Правила браков E8: ровно один объект, мод заменяет его целиком по тому же id.");
    s.required("ranks", Type::block(rank_schema()))
        .required("score", Type::block(score_schema()))
        .required("money", Type::block(money_schema()))
        .required("eligibility", Type::block(eligibility_schema()))
        .required("relation", Type::block(relation_schema()))
        .required("negotiation", Type::block(negotiation_schema()))
        .required("consequences", Type::block(consequences_schema()))
        .required("matchmaker", Type::block(matchmaker_schema()));
    set.add(std::move(s));
}

}  // namespace pat::data
