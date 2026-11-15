#include "pat/sim/marriage_rules.hpp"

#include "pat/sim/family/tables.hpp"

#include <format>
#include <string_view>
#include <utility>

namespace pat::sim {

namespace {

// ==================================================================================================
// THE TABLE OF DEFAULT VALUES
//
// The only numeric constants of the marriage evaluation in code. Every value is a [predl.] proposal
// of docs/dev/marriage-succession-model.md (section number next to the group); the corridors and
// caps marked "own" are required by the task (the dowry and the political gain must not buy every
// match) and chosen here so that one term never outweighs the rest of the score.
// ==================================================================================================
constexpr MarriageRules kDefaultMarriageRules{
    // GDD 01 section 8.1 (the same thresholds as `honor_ranks.pd`): impoverished, service, noble,
    // great, foremost. Specification finding F4.
    .ranks = {.min_honor = {{-1000, 0, 100, 500, 1000}}},
    // Specification section 8.4.
    .score =
        {
            .honor_weight_permille = 150,
            .honor_gap_limit = 600,
            .rank_step_penalty = 40,
            // 270 roubles of the standard dowry are about +-10 points. Own: the corridor is +-30
            // (the most a house can be paid to take the match).
            .dowry_points_per_100rub = 4,
            .dowry_points_cap = 30,
            .relation_weight_permille = 500,
            .opinion_weight_permille = 500,
            // 10010...10012: (health - 50) x 0.2, (fertility - 50) x 0.2 (the bride only),
            // (sum of the skills - 40) x 0.15.
            .health_weight_permille = 200,
            .health_neutral = 50,
            .fertility_weight_permille = 200,
            .fertility_neutral = 50,
            .skills_weight_permille = 150,
            .skills_neutral = 40,
            // 8 points per year above 10 years. Own: counted for at most 30 such years.
            .age_gap_penalty = 8,
            .age_gap_free_years = 10,
            .age_gap_years_cap = 30,
            // Own corridor of the hooks of E12/E15/E31 (they are 0 in M1).
            .political_gain_cap = 30,
            .faith_penalty_cap = 100,
            .accept_threshold = 0,
            .forecast_margin = 15,
        },
    // Specification section 8.3: 100 roubles + 0.5 rouble per point of honor, between 50 and 1000
    // roubles; the envoy costs 80 + 40 roubles, the dispensation of the Church 300 roubles more.
    .money =
        {
            .dowry_base_kop = 10'000,
            .dowry_per_honor_kop = 50,
            .dowry_min_kop = 5'000,
            .dowry_max_kop = 100'000,
            .envoy_road_kop = 8'000,
            .envoy_gifts_kop = 4'000,
            .dispensation_fee_kop = 30'000,
        },
    // Specification section 8.1 and decision #289: absolute blood ban through degree 4,
    // dispensable degrees 5-6, affinity through degree 4.
    .eligibility =
        {
            .min_marriage_age_male = 16,
            .min_marriage_age_female = 15,
            .min_betrothal_age = 7,
            .blood_forbidden_degree = 6,
            .blood_absolute_degree = 4,
            .affinity_forbidden_degree = 4,
            .mourning_days_female = 365,
            .mourning_days_male = 180,
            .require_opposite_sex = true,
            .forbid_same_house = true,
        },
    // Specification section 8.4.1: at most three current marriages contribute five points each.
    .relation = {.marriage_tie_bonus = 5, .max_marriage_ties = 3},
    // Specification sections 5, 7.3 and 8.3.
    .negotiation =
        {
            .answer_days = 30,
            .max_parallel_negotiations = 3,
            .refusal_cooldown_days = 365,
            .withdraw_refund_permille = 0,
        },
    // Specification sections 5 and 8.8. Penalties are magnitudes, not signed changes.
    .consequences =
        {
            .dispensation_honor = 10,
            .marriage_accepted_opinion = 5,
            .marriage_refused_opinion = 5,
            .betrothal_broken_honor = 10,
            .betrothal_broken_opinion = 25,
            .tonsure_honor = 30,
            .tonsure_unity = 5,
            .tonsure_opinion = 40,
            .tonsure_refund_permille = 500,
            .third_marriage_honor = 15,
            .wedding_rank_honor = 5,
            .married_into_opinion = 10,
            .widow_claim_permille = 1500,
            .bride_joins_groom_house = true,
        },
    // Specification section 8.9.
    .matchmaker = {.matchmaker_min_value = 0, .ai_may_propose_to_player = false},
};

// ---- validation helpers ------------------------------------------------------------------------

using Check = core::Result<void, MarriageRulesError>;

Check fail(std::string message, std::string field) {
    return Check::err(MarriageRulesError{std::move(message), std::move(field)});
}

Check in_range(std::string_view name, std::int64_t value, std::int64_t lo, std::int64_t hi) {
    if (value < lo || value > hi) {
        return fail(std::format("{} = {} is out of range {}...{}", name, value, lo, hi), std::string(name));
    }
    return Check::ok();
}

/// Largest weight in per-mille accepted (x10); keeps every product far from overflow.
constexpr std::int64_t kMaxWeight = 10'000;
/// Largest amount of points a term may be configured to.
constexpr std::int64_t kMaxPoints = 10'000;

#define PAT_MARRIAGE_CHECK(expr)                                                                             \
    do {                                                                                                     \
        if (auto res_ = (expr); !res_)                                                                       \
            return res_;                                                                                     \
    } while (false)

Check check_ranks(const MarriageRankRules& r) {
    for (std::size_t i = 0; i < r.min_honor.size(); ++i) {
        PAT_MARRIAGE_CHECK(
            in_range(std::format("ranks.min_honor[{}]", i), r.min_honor[i], -1'000'000, 1'000'000));
        if (i > 0 && r.min_honor[i] <= r.min_honor[i - 1]) {
            return fail(std::format("ranks.min_honor[{}] must be above ranks.min_honor[{}]", i, i - 1),
                        std::format("ranks.min_honor[{}]", i));
        }
    }
    return Check::ok();
}

Check check_score(const MarriageScoreRules& s) {
    PAT_MARRIAGE_CHECK(in_range("score.honor_weight_permille", s.honor_weight_permille, 0, kMaxWeight));
    PAT_MARRIAGE_CHECK(in_range("score.honor_gap_limit", s.honor_gap_limit, 0, 100'000));
    PAT_MARRIAGE_CHECK(in_range("score.rank_step_penalty", s.rank_step_penalty, 0, kMaxPoints));
    PAT_MARRIAGE_CHECK(in_range("score.dowry_points_per_100rub", s.dowry_points_per_100rub, 0, kMaxPoints));
    PAT_MARRIAGE_CHECK(in_range("score.dowry_points_cap", s.dowry_points_cap, 0, kMaxPoints));
    PAT_MARRIAGE_CHECK(in_range("score.relation_weight_permille", s.relation_weight_permille, 0, kMaxWeight));
    PAT_MARRIAGE_CHECK(in_range("score.opinion_weight_permille", s.opinion_weight_permille, 0, kMaxWeight));
    PAT_MARRIAGE_CHECK(in_range("score.health_weight_permille", s.health_weight_permille, 0, kMaxWeight));
    PAT_MARRIAGE_CHECK(in_range("score.health_neutral", s.health_neutral, 0, 100));
    PAT_MARRIAGE_CHECK(
        in_range("score.fertility_weight_permille", s.fertility_weight_permille, 0, kMaxWeight));
    PAT_MARRIAGE_CHECK(in_range("score.fertility_neutral", s.fertility_neutral, 0, 100));
    PAT_MARRIAGE_CHECK(in_range("score.skills_weight_permille", s.skills_weight_permille, 0, kMaxWeight));
    PAT_MARRIAGE_CHECK(in_range("score.skills_neutral", s.skills_neutral, 0, 100));
    PAT_MARRIAGE_CHECK(in_range("score.age_gap_penalty", s.age_gap_penalty, 0, kMaxPoints));
    PAT_MARRIAGE_CHECK(in_range("score.age_gap_free_years", s.age_gap_free_years, 0, 150));
    PAT_MARRIAGE_CHECK(in_range("score.age_gap_years_cap", s.age_gap_years_cap, 0, 150));
    PAT_MARRIAGE_CHECK(in_range("score.political_gain_cap", s.political_gain_cap, 0, kMaxPoints));
    PAT_MARRIAGE_CHECK(in_range("score.faith_penalty_cap", s.faith_penalty_cap, 0, kMaxPoints));
    PAT_MARRIAGE_CHECK(in_range("score.accept_threshold", s.accept_threshold, -100'000, 100'000));
    PAT_MARRIAGE_CHECK(in_range("score.forecast_margin", s.forecast_margin, 0, 100'000));
    return Check::ok();
}

Check check_money(const MarriageMoneyRules& m) {
    constexpr economy::Kopecks kMax = economy::kMaxPostingKop;
    PAT_MARRIAGE_CHECK(in_range("money.dowry_base_kop", m.dowry_base_kop, 0, kMax));
    PAT_MARRIAGE_CHECK(in_range("money.dowry_per_honor_kop", m.dowry_per_honor_kop, 0, 1'000'000'000));
    PAT_MARRIAGE_CHECK(in_range("money.dowry_min_kop", m.dowry_min_kop, 0, kMax));
    PAT_MARRIAGE_CHECK(in_range("money.dowry_max_kop", m.dowry_max_kop, 0, kMax));
    if (m.dowry_min_kop > m.dowry_max_kop) {
        return fail("money.dowry_min_kop must not exceed money.dowry_max_kop", "money.dowry_min_kop");
    }
    PAT_MARRIAGE_CHECK(in_range("money.envoy_road_kop", m.envoy_road_kop, 0, kMax / 4));
    PAT_MARRIAGE_CHECK(in_range("money.envoy_gifts_kop", m.envoy_gifts_kop, 0, kMax / 4));
    PAT_MARRIAGE_CHECK(in_range("money.dispensation_fee_kop", m.dispensation_fee_kop, 0, kMax / 4));
    return Check::ok();
}

Check check_eligibility(const MarriageEligibilityRules& e) {
    PAT_MARRIAGE_CHECK(in_range("eligibility.min_marriage_age_male", e.min_marriage_age_male, 1, 150));
    PAT_MARRIAGE_CHECK(in_range("eligibility.min_marriage_age_female", e.min_marriage_age_female, 1, 150));
    PAT_MARRIAGE_CHECK(in_range("eligibility.min_betrothal_age", e.min_betrothal_age, 1, 150));
    if (e.min_betrothal_age > e.min_marriage_age_male || e.min_betrothal_age > e.min_marriage_age_female) {
        return fail("eligibility.min_betrothal_age must not exceed either minimum marriage age",
                    "eligibility.min_betrothal_age");
    }
    PAT_MARRIAGE_CHECK(in_range("eligibility.blood_forbidden_degree", e.blood_forbidden_degree, 0, 6));
    PAT_MARRIAGE_CHECK(in_range("eligibility.blood_absolute_degree", e.blood_absolute_degree, 0, 6));
    PAT_MARRIAGE_CHECK(in_range("eligibility.affinity_forbidden_degree", e.affinity_forbidden_degree, 0, 6));
    if (e.blood_absolute_degree > e.blood_forbidden_degree) {
        return fail("eligibility.blood_absolute_degree must not exceed blood_forbidden_degree",
                    "eligibility.blood_absolute_degree");
    }
    PAT_MARRIAGE_CHECK(in_range("eligibility.mourning_days_female", e.mourning_days_female, 0, 36'500));
    PAT_MARRIAGE_CHECK(in_range("eligibility.mourning_days_male", e.mourning_days_male, 0, 36'500));
    return Check::ok();
}

Check check_relation(const MarriageRelationRules& r) {
    PAT_MARRIAGE_CHECK(in_range("relation.marriage_tie_bonus", r.marriage_tie_bonus, 0, 100));
    PAT_MARRIAGE_CHECK(in_range("relation.max_marriage_ties", r.max_marriage_ties, 0, 1'000));
    return Check::ok();
}

Check check_negotiation(const MarriageNegotiationRules& n) {
    PAT_MARRIAGE_CHECK(in_range("negotiation.answer_days", n.answer_days, 1, 36'500));
    PAT_MARRIAGE_CHECK(in_range("negotiation.max_parallel_negotiations",
                                n.max_parallel_negotiations,
                                1,
                                static_cast<std::int64_t>(family::kMaxNegotiations)));
    PAT_MARRIAGE_CHECK(in_range("negotiation.refusal_cooldown_days", n.refusal_cooldown_days, 0, 36'500));
    PAT_MARRIAGE_CHECK(in_range("negotiation.withdraw_refund_permille", n.withdraw_refund_permille, 0, 1000));
    return Check::ok();
}

Check check_consequences(const MarriageConsequenceRules& c) {
    PAT_MARRIAGE_CHECK(in_range("consequences.dispensation_honor", c.dispensation_honor, 0, kMaxPoints));
    PAT_MARRIAGE_CHECK(
        in_range("consequences.marriage_accepted_opinion", c.marriage_accepted_opinion, 0, 100));
    PAT_MARRIAGE_CHECK(in_range("consequences.marriage_refused_opinion", c.marriage_refused_opinion, 0, 100));
    PAT_MARRIAGE_CHECK(
        in_range("consequences.betrothal_broken_honor", c.betrothal_broken_honor, 0, kMaxPoints));
    PAT_MARRIAGE_CHECK(in_range("consequences.betrothal_broken_opinion", c.betrothal_broken_opinion, 0, 100));
    PAT_MARRIAGE_CHECK(in_range("consequences.tonsure_honor", c.tonsure_honor, 0, kMaxPoints));
    PAT_MARRIAGE_CHECK(in_range("consequences.tonsure_unity", c.tonsure_unity, 0, 100));
    PAT_MARRIAGE_CHECK(in_range("consequences.tonsure_opinion", c.tonsure_opinion, 0, 100));
    PAT_MARRIAGE_CHECK(in_range("consequences.tonsure_refund_permille", c.tonsure_refund_permille, 0, 1000));
    PAT_MARRIAGE_CHECK(in_range("consequences.third_marriage_honor", c.third_marriage_honor, 0, kMaxPoints));
    PAT_MARRIAGE_CHECK(in_range("consequences.wedding_rank_honor", c.wedding_rank_honor, 0, kMaxPoints));
    PAT_MARRIAGE_CHECK(in_range("consequences.married_into_opinion", c.married_into_opinion, 0, 100));
    PAT_MARRIAGE_CHECK(in_range("consequences.widow_claim_permille", c.widow_claim_permille, 0, kMaxWeight));
    return Check::ok();
}

Check check_matchmaker(const MarriageMatchmakerRules& m) {
    return in_range("matchmaker.matchmaker_min_value", m.matchmaker_min_value, -100'000, 100'000);
}

#undef PAT_MARRIAGE_CHECK

}  // namespace

MarriageRules MarriageRules::defaults() {
    return kDefaultMarriageRules;
}

core::Result<void, MarriageRulesError> validate(const MarriageRules& rules) {
    if (auto res = check_ranks(rules.ranks); !res) {
        return res;
    }
    if (auto res = check_score(rules.score); !res) {
        return res;
    }
    if (auto res = check_money(rules.money); !res) {
        return res;
    }
    if (auto res = check_eligibility(rules.eligibility); !res) {
        return res;
    }
    if (auto res = check_relation(rules.relation); !res) {
        return res;
    }
    if (auto res = check_negotiation(rules.negotiation); !res) {
        return res;
    }
    if (auto res = check_consequences(rules.consequences); !res) {
        return res;
    }
    return check_matchmaker(rules.matchmaker);
}

}  // namespace pat::sim
