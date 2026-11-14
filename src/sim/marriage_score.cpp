#include "pat/sim/marriage_score.hpp"

#include <algorithm>
#include <cstdlib>

namespace pat::sim {

namespace {

using economy::Breakdown;
using economy::Kopecks;
using economy::NodeIndex;
using economy::Unit;

constexpr std::int64_t kHundredRoublesKop = 10'000;
constexpr std::int32_t kMaxOpinionPoints = 100;
constexpr std::int32_t kMaxRelationPoints = 100;

[[nodiscard]] constexpr std::int32_t
clamp_i32(std::int64_t value, std::int64_t lo, std::int64_t hi) noexcept {
    return static_cast<std::int32_t>(std::clamp(value, lo, hi));
}

/// value x permille / 1000, truncated toward zero. Callers keep value and permille within the ranges
/// the rules validate, so the product cannot overflow.
[[nodiscard]] constexpr std::int32_t scale_permille(std::int64_t value, std::int64_t permille) noexcept {
    return static_cast<std::int32_t>(value * permille / 1000);
}

[[nodiscard]] constexpr std::int32_t whole(core::Fixed32 value) noexcept {
    return static_cast<std::int32_t>(value.to_int());
}

/// The living head of a house, or an invalid id.
[[nodiscard]] CharacterId living_head(const World& world, DynastyId house) noexcept {
    if (!world.exists(house)) {
        return CharacterId{};
    }
    const CharacterId head = world.dynasties().head[house.index()];
    return head.valid() && world.is_alive(head) ? head : CharacterId{};
}

[[nodiscard]] std::optional<MatchParty>
make_party(const World& world, const MarriageRules& rules, CharacterId person, DynastyId proposer) {
    if (!person.valid() || !world.is_alive(person)) {
        return std::nullopt;
    }
    const auto& characters = world.characters();
    const std::size_t index = person.index();
    MatchParty party;
    party.person = person;
    party.house = characters.dynasty[index];
    if (!world.exists(party.house)) {
        return std::nullopt;
    }
    party.honor = world.dynasties().honor[party.house.index()];
    party.rank = honor_rank(rules, party.honor);
    party.age = age_years(characters.birth_date[index], world.today());
    party.health = clamp_i32(whole(characters.health[index]), 0, 100);
    party.fertility = clamp_i32(whole(characters.fertility[index]), 0, 100);
    std::int32_t skills = 0;
    for (const auto& skill : characters.skills[index]) {
        skills += whole(skill);
    }
    party.skills = clamp_i32(skills, 0, 100);
    if (proposer.valid() && party.house != proposer) {
        const CharacterId own_head = living_head(world, party.house);
        const CharacterId proposer_head = living_head(world, proposer);
        if (own_head.valid() && proposer_head.valid()) {
            party.opinion_of_proposer = clamp_i32(
                whole(world.get_opinion(own_head, proposer_head)), -kMaxOpinionPoints, kMaxOpinionPoints);
        }
    }
    return party;
}

/// accept_score of the match seen from `side` (specification section 8.4).
[[nodiscard]] AcceptScore compute_score(const MarriageRules& rules,
                                        const MatchParty& groom,
                                        const MatchParty& bride,
                                        Kopecks dowry_kop,
                                        const MatchContext& context,
                                        MatchSide side) {
    const MarriageScoreRules& r = rules.score;
    const bool groom_side = side == MatchSide::Groom;
    const MatchParty& own = groom_side ? groom : bride;
    const MatchParty& other = groom_side ? bride : groom;
    const auto side_index = static_cast<std::size_t>(side);

    AcceptScore result;
    auto& t = result.terms;

    // 0 honor_gap: the honor of the other house against the own one, clamped, x weight.
    const std::int32_t honor_gap =
        clamp_i32(static_cast<std::int64_t>(other.honor) - own.honor, -r.honor_gap_limit, r.honor_gap_limit);
    t[static_cast<std::size_t>(ScoreTerm::HonorGap)] = scale_permille(honor_gap, r.honor_weight_permille);

    // 1 rank_gap: a penalty for every rank the own house stands above the other.
    t[static_cast<std::size_t>(ScoreTerm::RankGap)] =
        -r.rank_step_penalty * std::max(0, own.rank - other.rank);

    // 2 dowry: the groom's house receives it (+), the bride's house pays it (-); inside a corridor.
    const Kopecks dowry = std::clamp<Kopecks>(dowry_kop, 0, economy::kMaxPostingKop);
    const std::int32_t dowry_points =
        clamp_i32(dowry * r.dowry_points_per_100rub / kHundredRoublesKop, 0, r.dowry_points_cap);
    t[static_cast<std::size_t>(ScoreTerm::Dowry)] = groom_side ? dowry_points : -dowry_points;

    // 3 relation of the two houses.
    t[static_cast<std::size_t>(ScoreTerm::Relation)] = scale_permille(
        clamp_i32(context.relation, -kMaxRelationPoints, kMaxRelationPoints), r.relation_weight_permille);

    // 4 opinion of the head of the answering house about the proposing house.
    t[static_cast<std::size_t>(ScoreTerm::OpinionOfProposer)] =
        scale_permille(own.opinion_of_proposer, r.opinion_weight_permille);

    // 5 member_quality: the person the house takes in (health, fertility of the bride, skills).
    result.health_part = scale_permille(other.health - r.health_neutral, r.health_weight_permille);
    result.fertility_part =
        groom_side ? scale_permille(other.fertility - r.fertility_neutral, r.fertility_weight_permille) : 0;
    result.skills_part = scale_permille(other.skills - r.skills_neutral, r.skills_weight_permille);
    t[static_cast<std::size_t>(ScoreTerm::MemberQuality)] =
        result.health_part + result.fertility_part + result.skills_part;

    // 6 age_gap: a penalty per year of the gap above the free years (counted up to a cap).
    const std::int32_t gap = std::abs(groom.age - bride.age);
    const std::int32_t excess =
        clamp_i32(static_cast<std::int64_t>(gap) - r.age_gap_free_years, 0, r.age_gap_years_cap);
    t[static_cast<std::size_t>(ScoreTerm::AgeGap)] = -r.age_gap_penalty * excess;

    // 7 external: the hooks of E12/E15 and E31 (0 in M1), both inside their corridors.
    result.political_gain_part =
        clamp_i32(context.political_gain[side_index], -r.political_gain_cap, r.political_gain_cap);
    result.faith_part = -clamp_i32(context.faith_penalty[side_index], 0, r.faith_penalty_cap);
    t[static_cast<std::size_t>(ScoreTerm::External)] = result.political_gain_part + result.faith_part;

    result.score = result.sum_of_terms();
    result.threshold = r.accept_threshold;
    result.accepted = result.score >= result.threshold;
    if (!result.accepted) {
        // The term that pulls the score down most: the lowest value, the first one on a tie.
        std::size_t weakest = 0;
        for (std::size_t i = 1; i < kScoreTermCount; ++i) {
            if (t[i] < t[weakest]) {
                weakest = i;
            }
        }
        result.refusal_code = score_term_code(static_cast<ScoreTerm>(weakest));
    }
    return result;
}

struct TermKey {
    std::uint16_t code;
    std::string_view key;
};

// One row per issued code, ascending by code (checked by a test). Keys are localisation keys.
constexpr std::array kMarriageTermKeys{
    TermKey{marriage_term::kAcceptScore, "marriage.score.total"},
    TermKey{marriage_term::kHonorGap, "marriage.score.honor_gap"},
    TermKey{marriage_term::kRankGap, "marriage.score.rank_gap"},
    TermKey{marriage_term::kDowry, "marriage.score.dowry"},
    TermKey{marriage_term::kRelation, "marriage.score.relation"},
    TermKey{marriage_term::kOpinionOfProposer, "marriage.score.opinion_of_proposer"},
    TermKey{marriage_term::kMemberQuality, "marriage.score.member_quality"},
    TermKey{marriage_term::kAgeGap, "marriage.score.age_gap"},
    TermKey{marriage_term::kExternal, "marriage.score.external"},
    TermKey{marriage_term::kPoliticalGain, "marriage.score.political_gain"},
    TermKey{marriage_term::kHealth, "marriage.score.health"},
    TermKey{marriage_term::kFertility, "marriage.score.fertility"},
    TermKey{marriage_term::kSkills, "marriage.score.skills"},
    TermKey{marriage_term::kFaithPenalty, "marriage.score.faith_penalty"},
    TermKey{marriage_term::kEnvoyTotal, "marriage.envoy.total"},
    TermKey{marriage_term::kEnvoyRoad, "marriage.envoy.road"},
    TermKey{marriage_term::kEnvoyGifts, "marriage.envoy.gifts"},
    TermKey{marriage_term::kEnvoyDispensation, "marriage.envoy.dispensation"},
    TermKey{marriage_term::kDowryTotal, "marriage.dowry.total"},
    TermKey{marriage_term::kDowryBase, "marriage.dowry.base"},
    TermKey{marriage_term::kDowryHonor, "marriage.dowry.honor"},
    TermKey{marriage_term::kDowryWidowClaim, "marriage.dowry.widow_claim"},
    TermKey{marriage_term::kDowryLimit, "marriage.dowry.limit"},
};

}  // namespace

std::string_view marriage_term_key(std::uint16_t code) noexcept {
    for (const auto& row : kMarriageTermKeys) {
        if (row.code == code) {
            return row.key;
        }
    }
    return {};
}

std::int32_t AcceptScore::sum_of_terms() const noexcept {
    std::int32_t sum = 0;
    for (const std::int32_t term : terms) {
        sum += term;
    }
    return sum;
}

std::int32_t AcceptScore::public_sum() const noexcept {
    std::int32_t sum = 0;
    for (std::size_t i = 0; i < kScoreTermCount; ++i) {
        if (is_public_term(static_cast<ScoreTerm>(i))) {
            sum += terms[i];
        }
    }
    return sum;
}

std::int32_t honor_rank(const MarriageRules& rules, std::int32_t honor) noexcept {
    std::int32_t rank = 0;
    for (std::size_t i = 1; i < rules.ranks.min_honor.size(); ++i) {
        if (honor >= rules.ranks.min_honor[i]) {
            rank = static_cast<std::int32_t>(i);
        }
    }
    return rank;
}

std::optional<MatchEvaluation> evaluate_match(const World& world,
                                              const MarriageRules& rules,
                                              CharacterId groom,
                                              CharacterId bride,
                                              Kopecks dowry_kop,
                                              const MatchContext& context) {
    auto groom_party = make_party(world, rules, groom, context.initiator);
    auto bride_party = make_party(world, rules, bride, context.initiator);
    if (!groom_party || !bride_party) {
        return std::nullopt;
    }
    MatchEvaluation evaluation;
    evaluation.groom = *groom_party;
    evaluation.bride = *bride_party;
    evaluation.dowry_kop = std::max<Kopecks>(dowry_kop, 0);
    evaluation.context = context;
    for (const MatchSide side : {MatchSide::Groom, MatchSide::Bride}) {
        evaluation.scores[static_cast<std::size_t>(side)] =
            compute_score(rules, evaluation.groom, evaluation.bride, evaluation.dowry_kop, context, side);
    }
    return evaluation;
}

const AcceptScore& accept_score(const MatchEvaluation& evaluation, MatchSide side, Breakdown* explain) {
    const AcceptScore& score = evaluation.score(side);
    if (explain != nullptr) {
        explain_accept_score(score, *explain);
    }
    return score;
}

void explain_accept_score(const AcceptScore& score, Breakdown& explain) {
    explain.begin(marriage_term::kAcceptScore, Unit::Points, score.score);
    for (std::size_t i = 0; i < kScoreTermCount; ++i) {
        const auto term = static_cast<ScoreTerm>(i);
        const NodeIndex node = explain.add_term(economy::kRootNode, score_term_code(term), score.terms[i]);
        if (node == economy::kNoNode) {
            continue;
        }
        if (term == ScoreTerm::MemberQuality) {
            static_cast<void>(explain.add_term(node, marriage_term::kHealth, score.health_part));
            static_cast<void>(explain.add_term(node, marriage_term::kFertility, score.fertility_part));
            static_cast<void>(explain.add_term(node, marriage_term::kSkills, score.skills_part));
        } else if (term == ScoreTerm::External) {
            static_cast<void>(
                explain.add_term(node, marriage_term::kPoliticalGain, score.political_gain_part));
            static_cast<void>(explain.add_term(node, marriage_term::kFaithPenalty, score.faith_part));
        }
    }
}

Forecast forecast_of(const MarriageRules& rules, const AcceptScore& score) noexcept {
    const std::int32_t value = score.public_sum();
    if (value >= score.threshold + rules.score.forecast_margin) {
        return Forecast::Likely;
    }
    if (value < score.threshold - rules.score.forecast_margin) {
        return Forecast::Unlikely;
    }
    return Forecast::Unclear;
}

TermBounds term_bounds(const MarriageRules& rules, ScoreTerm term) noexcept {
    const MarriageScoreRules& r = rules.score;
    switch (term) {
        case ScoreTerm::HonorGap: {
            const std::int32_t limit = scale_permille(r.honor_gap_limit, r.honor_weight_permille);
            return {-limit, limit};
        }
        case ScoreTerm::RankGap:
            return {-r.rank_step_penalty * static_cast<std::int32_t>(kMarriageRankCount - 1), 0};
        case ScoreTerm::Dowry:
            return {-r.dowry_points_cap, r.dowry_points_cap};
        case ScoreTerm::Relation: {
            const std::int32_t limit = scale_permille(kMaxRelationPoints, r.relation_weight_permille);
            return {-limit, limit};
        }
        case ScoreTerm::OpinionOfProposer: {
            const std::int32_t limit = scale_permille(kMaxOpinionPoints, r.opinion_weight_permille);
            return {-limit, limit};
        }
        case ScoreTerm::MemberQuality: {
            // Health and fertility 0...100 and the sum of the skills 0...100 (parts truncate toward zero).
            TermBounds bounds;
            bounds.min = scale_permille(-r.health_neutral, r.health_weight_permille) +
                         scale_permille(-r.fertility_neutral, r.fertility_weight_permille) +
                         scale_permille(-r.skills_neutral, r.skills_weight_permille);
            bounds.max = scale_permille(100 - r.health_neutral, r.health_weight_permille) +
                         scale_permille(100 - r.fertility_neutral, r.fertility_weight_permille) +
                         scale_permille(100 - r.skills_neutral, r.skills_weight_permille);
            return bounds;
        }
        case ScoreTerm::AgeGap:
            return {-r.age_gap_penalty * r.age_gap_years_cap, 0};
        case ScoreTerm::External:
            return {-r.political_gain_cap - r.faith_penalty_cap, r.political_gain_cap};
    }
    return {};
}

TermBounds score_bounds(const MarriageRules& rules) noexcept {
    TermBounds total;
    for (std::size_t i = 0; i < kScoreTermCount; ++i) {
        const TermBounds bounds = term_bounds(rules, static_cast<ScoreTerm>(i));
        total.min += bounds.min;
        total.max += bounds.max;
    }
    return total;
}

Kopecks
standard_dowry(const World& world, const MarriageRules& rules, DynastyId bride_house, Breakdown* explain) {
    const MarriageMoneyRules& m = rules.money;
    const std::int32_t honor = world.exists(bride_house) ? world.dynasties().honor[bride_house.index()] : 0;
    const Kopecks surcharge = static_cast<Kopecks>(std::max(0, honor)) * m.dowry_per_honor_kop;
    const Kopecks dowry = std::clamp<Kopecks>(m.dowry_base_kop + surcharge, m.dowry_min_kop, m.dowry_max_kop);
    if (explain != nullptr) {
        explain->begin(marriage_term::kDowryTotal, Unit::Kopecks, dowry);
        static_cast<void>(explain->add_term(economy::kRootNode, marriage_term::kDowryBase, m.dowry_base_kop));
        static_cast<void>(explain->add_term(economy::kRootNode, marriage_term::kDowryHonor, surcharge));
        static_cast<void>(explain->add_remainder(economy::kRootNode, marriage_term::kDowryLimit));
    }
    return dowry;
}

Kopecks envoy_cost(const MarriageRules& rules, bool dispensation, Breakdown* explain) {
    const MarriageMoneyRules& m = rules.money;
    const Kopecks fee = dispensation ? m.dispensation_fee_kop : 0;
    const Kopecks cost = m.envoy_road_kop + m.envoy_gifts_kop + fee;
    if (explain != nullptr) {
        explain->begin(marriage_term::kEnvoyTotal, Unit::Kopecks, cost);
        static_cast<void>(explain->add_term(economy::kRootNode, marriage_term::kEnvoyRoad, m.envoy_road_kop));
        static_cast<void>(
            explain->add_term(economy::kRootNode, marriage_term::kEnvoyGifts, m.envoy_gifts_kop));
        static_cast<void>(explain->add_term(economy::kRootNode, marriage_term::kEnvoyDispensation, fee));
    }
    return cost;
}

}  // namespace pat::sim
