#include "pat/sim/marriage_rules.hpp"

#include <format>
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
};

// ---- validation helpers ------------------------------------------------------------------------

using Check = core::Result<void, MarriageRulesError>;

Check fail(std::string message) {
    return Check::err(MarriageRulesError{std::move(message)});
}

Check in_range(const char* name, std::int64_t value, std::int64_t lo, std::int64_t hi) {
    if (value < lo || value > hi) {
        return fail(std::format("{} = {} is out of range {}...{}", name, value, lo, hi));
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
        PAT_MARRIAGE_CHECK(in_range("ranks.min_honor", r.min_honor[i], -1'000'000, 1'000'000));
        if (i > 0 && r.min_honor[i] <= r.min_honor[i - 1]) {
            return fail(std::format("ranks.min_honor[{}] must be above ranks.min_honor[{}]", i, i - 1));
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
        return fail("money.dowry_min_kop must not exceed money.dowry_max_kop");
    }
    PAT_MARRIAGE_CHECK(in_range("money.envoy_road_kop", m.envoy_road_kop, 0, kMax / 4));
    PAT_MARRIAGE_CHECK(in_range("money.envoy_gifts_kop", m.envoy_gifts_kop, 0, kMax / 4));
    PAT_MARRIAGE_CHECK(in_range("money.dispensation_fee_kop", m.dispensation_fee_kop, 0, kMax / 4));
    return Check::ok();
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
    return check_money(rules.money);
}

}  // namespace pat::sim
