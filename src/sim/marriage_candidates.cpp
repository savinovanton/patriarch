#include "pat/sim/marriage_candidates.hpp"

#include "pat/sim/dynasty_relation.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace pat::sim {

namespace {

/// Fold only ASCII and Russian UTF-8 letters; preserve every other byte exactly.
[[nodiscard]] std::string fold_name(std::string_view text) {
    std::string out(text);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto first = static_cast<unsigned char>(out[i]);
        if (first >= 'A' && first <= 'Z') {
            out[i] = static_cast<char>(first + ('a' - 'A'));
        } else if (first == 0xd0 && i + 1 < out.size()) {
            const auto second = static_cast<unsigned char>(out[i + 1]);
            if (second == 0x81) {  // U+0401 -> U+0451 (Yo)
                out[i] = static_cast<char>(0xd1);
                out[++i] = static_cast<char>(0x91);
            } else if (second >= 0x90 && second <= 0x9f) {
                out[++i] = static_cast<char>(second + 0x20);
            } else if (second >= 0xa0 && second <= 0xaf) {
                out[i] = static_cast<char>(0xd1);
                out[++i] = static_cast<char>(second - 0x20);
            }
        }
    }
    return out;
}

[[nodiscard]] std::string_view name_of(const core::StringInterner& names, core::StringId id) {
    return id.valid() && id.index() < names.size() ? names.view(id) : std::string_view{};
}

struct HouseFacts {
    std::optional<MatchHouse> match;
    std::string_view faith;
    std::string name;
    std::optional<std::int32_t> relation;
    economy::Kopecks dowry_kop = 0;
    std::optional<MatchEvaluation> evaluation;
    std::int32_t responder_public_without_age = 0;
};

struct CandidateOrder {
    std::uint64_t key = 0;
    std::size_t source = 0;
};

}  // namespace

CandidateList find_candidates(const World& world, const MarriageRules& rules, const CandidateQuery& query) {
    CandidateList out;
    if (!world.dynasty_mode() || !world.is_alive(query.member) ||
        (query.min_age && query.max_age && *query.min_age > *query.max_age) ||
        (query.min_rank && query.max_rank && *query.min_rank > *query.max_rank) ||
        (!query.text.empty() && query.names == nullptr)) {
        return out;
    }
    const auto& people = world.characters();
    const auto& houses = world.dynasties();
    const auto member_house = people.dynasty[query.member.index()];
    if (!member_house.valid() || member_house.index() >= houses.size() ||
        (query.house.valid() && query.house.index() >= houses.size())) {
        return out;
    }

    const FaithPolicy default_faith;
    const auto& faith = query.faith_policy != nullptr ? *query.faith_policy : default_faith;
    const std::string needle = fold_name(query.text);
    std::vector<HouseFacts> house_facts(houses.size());
    for (std::size_t i = 0; i < houses.size(); ++i) {
        auto& facts = house_facts[i];
        const DynastyId house(static_cast<std::uint32_t>(i));
        facts.match = prepare_match_house(world, rules, house, member_house);
        facts.faith = faith.faith_of(house);
        facts.dowry_kop = standard_dowry(world, rules, house);
        if (!needle.empty()) {
            facts.name = fold_name(name_of(*query.names, houses.name[i]));
        }
    }

    const bool member_male = people.sex[query.member.index()] == Sex::Male;
    const Sex wanted_sex = query.sex.value_or(member_male ? Sex::Female : Sex::Male);
    const MatchSide member_side = member_male ? MatchSide::Groom : MatchSide::Bride;
    const MatchSide responder_side = member_male ? MatchSide::Bride : MatchSide::Groom;
    const MarriageCheckOptions options{query.as_betrothal, query.dispensation, member_house};
    MarriageCheckContext eligibility(world, rules, query.member, options, query.faith_policy);
    const auto& member_facts = house_facts[member_house.index()];
    if (!member_facts.match) {
        return out;
    }
    const auto member_party =
        prepare_match_party(world, query.member, *member_facts.match, eligibility.age_of(query.member));
    if (!member_party) {
        return out;
    }
    // Match family_marriage_of's latest non-Closed record once for the whole snapshot.
    // Candidate pools without widow records need no character-sized allocation.
    std::vector<const family::Marriage*> latest_marriages;
    const auto& marriages = world.family().marriages;
    if (std::ranges::any_of(marriages,
                            [](const auto& row) { return row.state == family::MarriageState::Widowed; })) {
        latest_marriages.resize(people.ids.capacity(), nullptr);
        for (const auto& marriage : marriages) {
            if (marriage.state == family::MarriageState::Closed) {
                continue;
            }
            latest_marriages[marriage.groom.index()] = &marriage;
            latest_marriages[marriage.bride.index()] = &marriage;
        }
    }
    out.rows.reserve(world.living().size() / 2);
    for (const auto person : world.living()) {
        const auto index = person.index();
        const auto house = people.dynasty[index];
        if (person == query.member || people.sex[index] != wanted_sex || !house.valid() ||
            house.index() >= houses.size() || (query.house.valid() && house != query.house)) {
            continue;
        }
        auto& facts = house_facts[house.index()];
        if (!facts.match) {
            continue;
        }
        const auto age = eligibility.age_of(person);
        const auto rank = facts.match->rank;
        if ((query.min_age && age < *query.min_age) || (query.max_age && age > *query.max_age) ||
            (query.min_rank && rank < *query.min_rank) || (query.max_rank && rank > *query.max_rank) ||
            (!query.faith.empty() && facts.faith != query.faith)) {
            continue;
        }
        if (!needle.empty() && facts.name.find(needle) == std::string::npos &&
            fold_name(name_of(*query.names, people.name[index])).find(needle) == std::string::npos) {
            continue;
        }

        MarriageCandidate row;
        row.person = person;
        row.house = house;
        row.age = age;
        row.rank = rank;
        row.faith = facts.faith;
        row.denials = eligibility.check(person, &row.kinship);
        if (query.unrelated_only &&
            (row.kinship.blood_degree != 0 || row.kinship.affinity_degree != 0 || row.kinship.spiritual)) {
            continue;
        }
        ++out.total;
        const bool available = row.available();
        out.available_count += available ? 1 : 0;
        if ((query.availability == CandidateAvailability::Available && !available) ||
            (query.availability == CandidateAvailability::Unavailable && available)) {
            continue;
        }

        row.denial = candidate_denial(row.denials);
        row.dispensable = row.kinship.blood_degree > rules.eligibility.blood_absolute_degree &&
                          row.kinship.blood_degree <= rules.eligibility.blood_forbidden_degree;
        if (!facts.relation) {
            facts.relation = dynasty_relation(world, member_house, house, rules);
        }
        row.relation = *facts.relation;
        const auto bride_id = member_male ? person : query.member;
        const auto groom_house = member_male ? member_house : house;
        const auto widow = latest_marriages.empty()
                               ? std::nullopt
                               : widow_dowry(latest_marriages[bride_id.index()], bride_id, groom_house);
        row.dowry_kop = widow ? widow->amount : member_male ? facts.dowry_kop : member_facts.dowry_kop;
        MatchContext context;
        context.initiator = member_house;
        context.relation = row.relation;
        const auto candidate_party = prepare_match_party(world, person, *facts.match, age);
        if (!candidate_party) {
            continue;  // The pool above guarantees living people with known houses.
        }
        const auto& groom = member_male ? *member_party : *candidate_party;
        const auto& bride = member_male ? *candidate_party : *member_party;
        if (widow) {
            // A widow's personal claim must neither reuse nor seed the ordinary per-house score.
            const auto evaluation = evaluate_prepared_match(rules, groom, bride, row.dowry_kop, context);
            row.score = evaluation.score(member_side);
            row.match_value = row.score.score;
            row.forecast = forecast_of(rules, evaluation.score(responder_side));
            out.rows.push_back(std::move(row));
            continue;
        }
        const bool first_for_house = !facts.evaluation;
        if (first_for_house) {
            facts.evaluation = evaluate_prepared_match(rules, groom, bride, row.dowry_kop, context);
            const auto& responder = facts.evaluation->score(responder_side);
            facts.responder_public_without_age =
                responder.public_sum() - responder.terms[static_cast<std::size_t>(ScoreTerm::AgeGap)];
        }
        row.score = first_for_house
                        ? facts.evaluation->score(member_side)
                        : reevaluate_match_score(
                              rules, facts.evaluation->score(member_side), groom, bride, member_side);
        row.match_value = row.score.score;
        // The age-gap term is public and symmetric; all other public terms are fixed per house.
        row.forecast = forecast_from_public_score(
            rules,
            facts.responder_public_without_age + row.score.terms[static_cast<std::size_t>(ScoreTerm::AgeGap)],
            facts.evaluation->score(responder_side).threshold);
        out.rows.push_back(std::move(row));
    }

    const auto key = [&](const MarriageCandidate& row) {
        switch (query.sort) {
            case CandidateSort::Relation:
                return row.relation;
            case CandidateSort::Age:
                return row.age;
            case CandidateSort::MatchValue:
                return row.match_value;
        }
        return row.match_value;
    };
    std::vector<CandidateOrder> order;
    order.reserve(out.rows.size());
    for (std::size_t i = 0; i < out.rows.size(); ++i) {
        const auto& row = out.rows[i];
        auto ordered_key = static_cast<std::uint32_t>(key(row)) ^ 0x80000000U;
        if (!query.ascending) {
            ordered_key = ~ordered_key;
        }
        order.push_back({(static_cast<std::uint64_t>(ordered_key) << 32U) | row.person.value(), i});
    }
    std::ranges::sort(order, {}, &CandidateOrder::key);
    // source maps each destination to its original row. Consume cycles with one saved payload;
    // writing a fixed point into source marks the destination after its next link was read.
    for (std::size_t start = 0; start < order.size(); ++start) {
        if (order[start].source == start) {
            continue;
        }
        MarriageCandidate saved = std::move(out.rows[start]);
        std::size_t destination = start;
        for (;;) {
            const auto source = order[destination].source;
            order[destination].source = destination;
            if (source == start) {
                out.rows[destination] = std::move(saved);
                break;
            }
            out.rows[destination] = std::move(out.rows[source]);
            destination = source;
        }
    }
    return out;
}

}  // namespace pat::sim
