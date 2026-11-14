#pragma once

#include "pat/core/result.hpp"
#include "pat/sim/economy/money.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// Marriage rules (E8-T05, docs/dev/marriage-succession-model.md sections 8.3, 8.4 and 11, GDD 01
// section 7): the numbers the evaluation of a match reads.
//
// Rules are CONTENT, not state: they are not part of the World and are passed to the pure functions
// of marriage_score.hpp. MarriageRules::defaults() is the ONLY place in the code with numeric
// constants of the marriage (marriage_rules.cpp, one named table); E8-T07 reproduces exactly these
// values in `data/balance/marriage.pd` and a test keeps the two equal. Later tasks of the epic add
// their own groups of fields (admissibility T04, the matchmaker T20) next to the ones here.
//
// Like DemographyRules and EconomyRules this is plain data plus pure functions. There is no floating
// point: weights are per-mille of one (1000 = x1.0), points are whole numbers, money is kopecks.
//
// ALL numbers in the defaults are [predl.] proposals from the specification; they are meant to be
// tuned (E8-T24) and are replaced from data by E8-T07.

namespace pat::sim {

/// Ranks of a house by honor (GDD 01 section 8.1): 0 impoverished ... 4 foremost. The thresholds of
/// the presentation ranks (`honor_ranks.pd`) are NOT part of the simulation hash (finding F4 of the
/// specification), so the marriage keeps its own copy of them.
inline constexpr std::size_t kMarriageRankCount = 5;

struct MarriageRankRules {
    /// Lowest honor of each rank; strictly increasing, the first entry is the lowest possible honor.
    std::array<std::int32_t, kMarriageRankCount> min_honor{};

    [[nodiscard]] bool operator==(const MarriageRankRules&) const = default;
};

/// The terms of accept_score (specification section 8.4).
struct MarriageScoreRules {
    /// Term 0 (honor_gap): the gap `honor_other - honor_own` is clamped to +-`honor_gap_limit` and
    /// multiplied by the weight.
    std::int32_t honor_weight_permille = 0;
    std::int32_t honor_gap_limit = 0;
    /// Term 1 (rank_gap): points lost for every rank the answering house stands above the other.
    std::int32_t rank_step_penalty = 0;
    /// Term 2 (dowry): points per 100 roubles, and the corridor of the term (+-`dowry_points_cap`), so
    /// that no money buys every match.
    std::int32_t dowry_points_per_100rub = 0;
    std::int32_t dowry_points_cap = 0;
    /// Terms 3 and 4: the weights of the relation of the houses and of the opinion of the head.
    std::int32_t relation_weight_permille = 0;
    std::int32_t opinion_weight_permille = 0;
    /// Term 5 (member_quality): weight and the neutral value of health, fertility (the bride only)
    /// and the sum of the five skills; a value above the neutral one is a plus.
    std::int32_t health_weight_permille = 0;
    std::int32_t health_neutral = 0;
    std::int32_t fertility_weight_permille = 0;
    std::int32_t fertility_neutral = 0;
    std::int32_t skills_weight_permille = 0;
    std::int32_t skills_neutral = 0;
    /// Term 6 (age_gap): points lost for every year of the age gap above `age_gap_free_years`, for at
    /// most `age_gap_years_cap` such years.
    std::int32_t age_gap_penalty = 0;
    std::int32_t age_gap_free_years = 0;
    std::int32_t age_gap_years_cap = 0;
    /// Term 7 (external): the hooks of E12/E15 (political gain, a corridor of +-`political_gain_cap`)
    /// and E31 (faith penalty, 0...`faith_penalty_cap`). Both are 0 in M1.
    std::int32_t political_gain_cap = 0;
    std::int32_t faith_penalty_cap = 0;
    /// The house agrees when the score is at least this many points.
    std::int32_t accept_threshold = 0;
    /// The forecast of the UI (section 8.4): the public terms within this margin of the threshold are
    /// "unclear".
    std::int32_t forecast_margin = 0;

    [[nodiscard]] bool operator==(const MarriageScoreRules&) const = default;
};

/// The dowry and the cost of the envoy (specification section 8.3).
struct MarriageMoneyRules {
    /// dowry = clamp(base + max(0, honor of the bride's house) x per_honor, min, max), kopecks.
    economy::Kopecks dowry_base_kop = 0;
    economy::Kopecks dowry_per_honor_kop = 0;
    economy::Kopecks dowry_min_kop = 0;
    economy::Kopecks dowry_max_kop = 0;
    /// envoy cost = road + gifts + (dispensation ? fee : 0), kopecks.
    economy::Kopecks envoy_road_kop = 0;
    economy::Kopecks envoy_gifts_kop = 0;
    economy::Kopecks dispensation_fee_kop = 0;

    [[nodiscard]] bool operator==(const MarriageMoneyRules&) const = default;
};

struct MarriageRules {
    MarriageRankRules ranks;
    MarriageScoreRules score;
    MarriageMoneyRules money;

    /// The default rules: the one table of numeric constants (marriage_rules.cpp).
    [[nodiscard]] static MarriageRules defaults();

    [[nodiscard]] bool operator==(const MarriageRules&) const = default;
};

struct MarriageRulesError {
    std::string message;

    [[nodiscard]] bool operator==(const MarriageRulesError&) const = default;
};

/// Checks a rules instance for consistency and returns the first problem found: weights and amounts
/// within their ranges (so that no formula can overflow), rank thresholds strictly increasing, the
/// money corridor ordered (min <= max). Rules that pass can be used by the formulas without further
/// checks.
[[nodiscard]] core::Result<void, MarriageRulesError> validate(const MarriageRules& rules);

}  // namespace pat::sim
