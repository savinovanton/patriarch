#pragma once

#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/marriage_rules.hpp"
#include "pat/sim/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// Evaluation of a match (E8-T05, docs/dev/marriage-succession-model.md sections 8.3, 8.4 and 9,
// GDD 01 section 7.2): how good a marriage of two people is for each of the two houses, whether the
// answering house agrees, and why not.
//
// Everything here is a PURE function of the read-only World and the rules. There is no random draw
// (the answer of a house is a function of the state on the day of the answer, decision D4), no
// floating point (integers, truncation toward zero), and nothing is written to the World. The same
// code serves the simulation (the answer of a house, E8-T09), the matchmaker (E8-T20) and the UI
// (the tooltip of the score, the forecast, E8-T21): the Breakdown is built from the numbers the
// decision used, so a tooltip can never disagree with the decision.
//
// accept_score has eight terms (ScoreTerm) seen from the answering house; the score is their exact
// sum and the house agrees when it is at least the threshold of the rules. The dowry and the
// political gain are limited to a corridor of the rules, so money alone does not buy every match.
//
// Not here (other tasks): who may marry whom (check_marriage, E8-T04); the relation of two houses
// (dynasty_relation, E8-T06) and the hooks of E12/E15/E31 come in as numbers of MatchContext.

namespace pat::sim {

/// Which house answers. A match always has one groom (the husband, whose house RECEIVES the dowry)
/// and one bride (whose house PAYS it); the bride joins the house of the groom (decision D13).
enum class MatchSide : std::uint8_t { Groom = 0, Bride = 1 };
inline constexpr std::size_t kMatchSideCount = 2;

/// The terms of accept_score, in this order (numbers are stable, specification section 8.4).
enum class ScoreTerm : std::uint8_t {
    HonorGap = 0,           ///< the honor of the other house against the own one (clamped)
    RankGap = 1,            ///< a penalty for every rank the own house stands above the other
    Dowry = 2,              ///< + for the house that receives the dowry, - for the one that pays
    Relation = 3,           ///< the relation of the two houses (opinions of their members)
    OpinionOfProposer = 4,  ///< what the head of the answering house thinks of the proposing house
    MemberQuality = 5,      ///< health, fertility (the bride only) and skills of the other person
    AgeGap = 6,             ///< a penalty for the age gap above the free years
    External = 7,           ///< hooks: political gain (E12/E15) and the faith penalty (E31); 0 in M1
};
inline constexpr std::size_t kScoreTermCount = 8;

/// The public terms are known to the proposing side before the answer (the forecast of the UI uses
/// only them); the rest (4, 5 and 7) stays hidden until the house has answered.
[[nodiscard]] constexpr bool is_public_term(ScoreTerm term) noexcept {
    return term != ScoreTerm::OpinionOfProposer && term != ScoreTerm::MemberQuality &&
           term != ScoreTerm::External;
}

/// Stable codes of the Breakdown nodes of the marriage (specification section 9). Codes are never
/// reused or renumbered; new ones are appended to the end of their family.
namespace marriage_term {

// 10000: the score of a match
inline constexpr std::uint16_t kAcceptScore = 10000;  // the root: the score (Points)
inline constexpr std::uint16_t kHonorGap = 10001;
inline constexpr std::uint16_t kRankGap = 10002;
inline constexpr std::uint16_t kDowry = 10003;
inline constexpr std::uint16_t kRelation = 10004;
inline constexpr std::uint16_t kOpinionOfProposer = 10005;
inline constexpr std::uint16_t kMemberQuality = 10006;
inline constexpr std::uint16_t kAgeGap = 10007;
inline constexpr std::uint16_t kExternal = 10008;
inline constexpr std::uint16_t kPoliticalGain = 10009;  // child of kExternal
inline constexpr std::uint16_t kHealth = 10010;         // children of kMemberQuality
inline constexpr std::uint16_t kFertility = 10011;
inline constexpr std::uint16_t kSkills = 10012;
inline constexpr std::uint16_t kFaithPenalty = 10013;  // child of kExternal
// 10100: the cost of the envoy (Kopecks)
inline constexpr std::uint16_t kEnvoyTotal = 10100;
inline constexpr std::uint16_t kEnvoyRoad = 10101;
inline constexpr std::uint16_t kEnvoyGifts = 10102;
inline constexpr std::uint16_t kEnvoyDispensation = 10103;
// 10200: the dowry (Kopecks)
inline constexpr std::uint16_t kDowryTotal = 10200;
inline constexpr std::uint16_t kDowryBase = 10201;
inline constexpr std::uint16_t kDowryHonor = 10202;
inline constexpr std::uint16_t kDowryWidowClaim = 10203;  // reserved: the widow's claim (E8-T11)
inline constexpr std::uint16_t kDowryLimit = 10204;       // the clamp to the corridor of the rules

}  // namespace marriage_term

/// Breakdown code of a term of accept_score (10001...10008).
[[nodiscard]] constexpr std::uint16_t score_term_code(ScoreTerm term) noexcept {
    return static_cast<std::uint16_t>(marriage_term::kHonorGap + static_cast<std::uint16_t>(term));
}

/// Localisation key of a marriage Breakdown code ("marriage.score.honor_gap"), or an empty view for
/// a code that is not issued. The returned view refers to static storage.
[[nodiscard]] std::string_view marriage_term_key(std::uint16_t code) noexcept;

// ---- input and result --------------------------------------------------------------------------

/// What the evaluation cannot read from the World alone.
struct MatchContext {
    /// The house that sent the envoy. Invalid = nobody (the term of the opinion is 0).
    DynastyId initiator;
    /// The relation of the two houses, -100...100 (dynasty_relation of E8-T06; 0 until then).
    std::int32_t relation = 0;
    /// Hook of E12/E15: the political gain of the match for each house (points), indexed by MatchSide;
    /// 0 in M1. Clamped to the corridor of the rules.
    std::array<std::int32_t, kMatchSideCount> political_gain{};
    /// Hook of E31: the penalty of each house for the faith (a positive number of points), indexed by
    /// MatchSide; 0 in M1. Clamped to 0...faith_penalty_cap.
    std::array<std::int32_t, kMatchSideCount> faith_penalty{};

    [[nodiscard]] bool operator==(const MatchContext&) const = default;
};

/// One of the two people of a match and what the evaluation knows about them and their house.
struct MatchParty {
    CharacterId person;
    DynastyId house;
    std::int32_t honor = 0;      ///< honor of the house
    std::int32_t rank = 0;       ///< 0...kMarriageRankCount-1, by the thresholds of the rules
    std::int32_t age = 0;        ///< whole years today
    std::int32_t health = 0;     ///< whole points, 0...100
    std::int32_t fertility = 0;  ///< whole points, 0...100
    std::int32_t skills = 0;     ///< sum of the five skills, whole points, 0...100
    /// The opinion (whole points) the head of this house holds of the head of the proposing house;
    /// 0 when there is no proposer, the house is the proposer's, or one of the heads is missing.
    std::int32_t opinion_of_proposer = 0;

    [[nodiscard]] bool operator==(const MatchParty&) const = default;
};

/// accept_score as seen from one house.
struct AcceptScore {
    /// The terms in the order of ScoreTerm. `score` is exactly their sum.
    std::array<std::int32_t, kScoreTermCount> terms{};
    /// The parts of the term MemberQuality (their sum is that term) ...
    std::int32_t health_part = 0;
    std::int32_t fertility_part = 0;
    std::int32_t skills_part = 0;
    /// ... and of the term External (their sum is that term; the faith part is 0 or negative).
    std::int32_t political_gain_part = 0;
    std::int32_t faith_part = 0;

    std::int32_t score = 0;
    std::int32_t threshold = 0;
    /// score >= threshold.
    bool accepted = false;
    /// When refused: the Breakdown code (10001...10008) of the term with the lowest value (the first
    /// one on a tie), the reason shown to the player; 0 when the house agrees.
    std::uint16_t refusal_code = 0;

    [[nodiscard]] std::int32_t sum_of_terms() const noexcept;
    /// Sum of the public terms (forecast).
    [[nodiscard]] std::int32_t public_sum() const noexcept;
    [[nodiscard]] bool operator==(const AcceptScore&) const = default;
};

/// The evaluation of a match for both houses.
struct MatchEvaluation {
    MatchParty groom;
    MatchParty bride;
    /// The dowry the evaluation was made for (kopecks, never negative).
    economy::Kopecks dowry_kop = 0;
    MatchContext context;
    /// accept_score seen from the house of the groom and from the house of the bride.
    std::array<AcceptScore, kMatchSideCount> scores{};

    [[nodiscard]] const AcceptScore& score(MatchSide side) const noexcept {
        return scores[static_cast<std::size_t>(side)];
    }
    [[nodiscard]] const MatchParty& party(MatchSide side) const noexcept {
        return side == MatchSide::Groom ? groom : bride;
    }
    [[nodiscard]] bool operator==(const MatchEvaluation&) const = default;
};

/// The range of values a term can take with the given rules; `min <= value <= max` always holds.
struct TermBounds {
    std::int32_t min = 0;
    std::int32_t max = 0;
};

enum class Forecast : std::uint8_t {
    Likely = 0,    ///< the public terms are above the threshold by more than the margin
    Unclear = 1,   ///< within the margin
    Unlikely = 2,  ///< below the threshold by more than the margin
};

// ---- functions ---------------------------------------------------------------------------------

/// The rank of a house with this honor: the highest rank whose threshold the honor reaches.
[[nodiscard]] std::int32_t honor_rank(const MarriageRules& rules, std::int32_t honor) noexcept;

/// Evaluates the match of `groom` and `bride` with the given dowry for both houses. nullopt when one
/// of the two is not alive or has no house. Does NOT check whether the match is allowed (that is
/// check_marriage). Never draws random numbers.
[[nodiscard]] std::optional<MatchEvaluation> evaluate_match(const World& world,
                                                            const MarriageRules& rules,
                                                            CharacterId groom,
                                                            CharacterId bride,
                                                            economy::Kopecks dowry_kop,
                                                            const MatchContext& context = {});

/// accept_score of the match for the answering house `side`, optionally with its explanation: the
/// root (marriage_term::kAcceptScore, unit Points) with the eight terms, and under the terms
/// MemberQuality and External their parts. Without a Breakdown it is only the stored result.
[[nodiscard]] const AcceptScore&
accept_score(const MatchEvaluation& evaluation, MatchSide side, economy::Breakdown* explain = nullptr);

/// Builds the explanation of an AcceptScore (the same tree accept_score fills).
void explain_accept_score(const AcceptScore& score, economy::Breakdown& explain);

/// The forecast the proposing side may see before the answer: only the public terms, against the
/// threshold with the margin of the rules.
[[nodiscard]] Forecast forecast_of(const MarriageRules& rules, const AcceptScore& score) noexcept;

/// The range of one term (rules only; the term never leaves it).
[[nodiscard]] TermBounds term_bounds(const MarriageRules& rules, ScoreTerm term) noexcept;
/// The range of the whole score: the sums of the bounds of the terms.
[[nodiscard]] TermBounds score_bounds(const MarriageRules& rules) noexcept;

/// The standard dowry of a bride from `bride_house`: clamp(base + max(0, honor) x per_honor, min,
/// max). Optionally explained (root kDowryTotal in kopecks: base, the surcharge for honor and the
/// clamp). An unknown house pays the base.
[[nodiscard]] economy::Kopecks standard_dowry(const World& world,
                                              const MarriageRules& rules,
                                              DynastyId bride_house,
                                              economy::Breakdown* explain = nullptr);

/// The cost of sending the envoy: road + gifts + (dispensation ? fee : 0). Optionally explained
/// (root kEnvoyTotal in kopecks).
[[nodiscard]] economy::Kopecks
envoy_cost(const MarriageRules& rules, bool dispensation, economy::Breakdown* explain = nullptr);

}  // namespace pat::sim
