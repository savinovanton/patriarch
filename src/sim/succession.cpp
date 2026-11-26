#include "pat/sim/succession.hpp"

#include "pat/sim/demography.hpp"
#include "pat/sim/kinship.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <span>
#include <tuple>
#include <utility>

namespace pat::sim {
namespace {

enum class Priority { Son, Daughter, DaughterLine, Brother, Nephew, Other, Ineligible };

}  // namespace

CharacterId fallback_successor(const World& world, DynastyId dynasty, CharacterId predecessor) {
    if (!world.dynasty_mode() || !world.exists(dynasty) || !world.exists(predecessor)) {
        return {};
    }

    const auto& people = world.characters();
    if (people.dynasty[predecessor.index()] != dynasty) {
        return {};
    }
    const auto members = world.members_of(dynasty);
    if (members.empty()) {
        return {};
    }
    const auto adult = [&](CharacterId id) {
        return world.is_alive(id) && !world.is_tonsured(id) &&
               age_years(people.birth_date[id.index()], world.today()) >= kDefaultDemography.prime_from;
    };
    const bool has_adult_man = std::ranges::any_of(members, [&](CharacterId id) {
        return id != predecessor && adult(id) && people.sex[id.index()] == Sex::Male;
    });
    const auto is_child_of = [&](CharacterId child, CharacterId parent) {
        return world.exists(child) &&
               (people.father[child.index()] == parent || people.mother[child.index()] == parent);
    };
    const auto siblings = kin::siblings_of(world, predecessor);
    const auto is_sibling = [&](CharacterId id) { return std::ranges::find(siblings, id) != siblings.end(); };

    std::tuple<Priority, Date, CharacterId> best{Priority::Ineligible, {}, {}};
    for (const CharacterId id : members) {
        if (id == predecessor || !adult(id)) {
            continue;
        }
        const std::size_t index = id.index();
        const bool male = people.sex[index] == Sex::Male;
        if (!male && has_adult_man) {
            continue;
        }
        Priority priority = Priority::Other;
        if (is_child_of(id, predecessor)) {
            priority = male ? Priority::Son : Priority::Daughter;
        } else if (is_child_of(people.mother[index], predecessor)) {
            priority = Priority::DaughterLine;
        } else if (male && is_sibling(id)) {
            priority = Priority::Brother;
        } else if (male && (is_sibling(people.father[index]) || is_sibling(people.mother[index]))) {
            priority = Priority::Nephew;
        }
        best = std::min(best, std::tuple{priority, people.birth_date[index], id});
    }
    return std::get<CharacterId>(best);
}

namespace {

constexpr std::int64_t kPointScale = core::Fixed32::one_raw;
constexpr std::size_t kNotCandidate = std::numeric_limits<std::size_t>::max();
constexpr std::array kSuccessionTermKeys{
    economy::TermKey{succession_term::kCouncilScore, "succession.council.total"},
    economy::TermKey{succession_term::kSkills, "succession.council.skills"},
    economy::TermKey{succession_term::kAge, "succession.council.age"},
    economy::TermKey{succession_term::kKinship, "succession.council.kinship"},
    economy::TermKey{succession_term::kOpinion, "succession.council.opinion"},
    economy::TermKey{succession_term::kLoyalty, "succession.council.loyalty"},
    economy::TermKey{succession_term::kPiety, "succession.council.piety"},
    economy::TermKey{succession_term::kStatus, "succession.council.status"},
    economy::TermKey{succession_term::kClaimantScore, "succession.claimant.total"},
    economy::TermKey{succession_term::kClaimantCouncil, "succession.claimant.council"},
    economy::TermKey{succession_term::kClaimantLawful, "succession.claimant.lawful"},
    economy::TermKey{succession_term::kClaimantAppointment, "succession.claimant.appointment"},
    economy::TermKey{succession_term::kReformCost, "succession.reform.total"},
    economy::TermKey{succession_term::kReformHonor, "succession.reform.honor"},
    economy::TermKey{succession_term::kReformUnity, "succession.reform.unity"},
    economy::TermKey{succession_term::kReformMoney, "succession.reform.money"},
    economy::TermKey{succession_term::kReformOpinions, "succession.reform.opinions"},
};
static_assert(economy::is_sorted_term_table(kSuccessionTermKeys));

// The same conversion serves every fixed-point council term. Do not truncate individual skills
// before adding them: their fractional parts can together make a whole weighted point.
std::int32_t weighted_points(std::int64_t raw, std::int32_t weight) {
    return static_cast<std::int32_t>(raw * weight / (1000 * kPointScale));
}

bool adult(const World& world, const SuccessionRules& rules, CharacterId id) {
    return age_years(world.characters().birth_date[id.index()], world.today()) >= rules.adult_age;
}

CharacterId lineage_anchor(const World& world, DynastyId dynasty, CharacterId predecessor) {
    if (predecessor.valid()) {
        return predecessor;
    }
    const auto history = world.chronicle(dynasty);
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (it->kind != ChronicleKind::HeadChanged) {
            continue;
        }
        for (const CharacterId id : {it->b, it->a}) {
            if (world.exists(id) && !world.is_alive(id)) {
                return id;
            }
        }
    }
    return {};
}

/// Direct relation classes are authoritative for both the score and the lineage roots. The
/// traversal below additionally labels represented descendants by the branch they represent.
struct Relations {
    const World& world;
    CharacterId predecessor;
    std::vector<CharacterId> siblings;

    Relations(const World& source, CharacterId anchor)
        : world(source), predecessor(anchor), siblings(kin::siblings_of(source, anchor)) {}

    bool child_of(CharacterId child, CharacterId parent) const {
        const auto& t = world.characters();
        return world.exists(child) && parent.valid() &&
               (t.father[child.index()] == parent || t.mother[child.index()] == parent);
    }

    HeirReason classify(CharacterId id) const {
        const auto& t = world.characters();
        const bool male = t.sex[id.index()] == Sex::Male;
        if (child_of(id, predecessor)) {
            return male ? HeirReason::EldestSon : HeirReason::Daughter;
        }
        if (male && std::ranges::find(siblings, id) != siblings.end()) {
            return HeirReason::Brother;
        }
        if (male && std::ranges::any_of(siblings, [&](CharacterId p) { return child_of(id, p); })) {
            return HeirReason::Nephew;
        }
        return HeirReason::Kin;
    }
};

/// Zero opinions count in the denominator. Walk only living adult members and their stored
/// opinion rows, once per request, rather than querying every pair of members separately.
struct CouncilOpinions {
    std::vector<std::int64_t> sums;
    std::int64_t adults = 0;

    CouncilOpinions(const World& world, const SuccessionRules& rules, DynastyId dynasty)
        : sums(world.characters().dynasty.size()) {
        const auto& t = world.characters();
        for (const CharacterId from : world.members_of(dynasty)) {
            if (!adult(world, rules, from)) {
                continue;
            }
            ++adults;
            for (const auto& opinion : world.opinions_from(from)) {
                const CharacterId to = opinion.target;
                if (to != from && world.is_alive(to) && t.dynasty[to.index()] == dynasty) {
                    sums[to.index()] += opinion.total().raw();
                }
            }
        }
    }

    std::int32_t mean(const World& world, const SuccessionRules& rules, CharacterId id) const {
        const auto count = adults - (adult(world, rules, id) ? 1 : 0);
        return count == 0 ? 0 : static_cast<std::int32_t>(sums[id.index()] / (count * kPointScale));
    }
};

CouncilScore score_candidate(const World& world,
                             const SuccessionRules& rules,
                             const Relations& relations,
                             CharacterId id,
                             std::int32_t opinion_mean) {
    const auto& t = world.characters();
    const auto i = id.index();
    CouncilScore score;
    std::int64_t skills_raw = 0;
    for (const auto skill : t.skills[i]) {
        skills_raw += skill.raw();
    }
    score.terms[0] = weighted_points(skills_raw, rules.skills_weight_permille);
    const auto years =
        std::clamp(age_years(t.birth_date[i], world.today()) - rules.adult_age, 0, rules.age_years_cap);
    score.terms[1] = weighted_points(years * kPointScale, rules.age_weight_permille);
    switch (relations.classify(id)) {
        case HeirReason::EldestSon:
            score.terms[2] = rules.son_bonus;
            break;
        case HeirReason::Daughter:
            score.terms[2] = rules.daughter_bonus;
            break;
        case HeirReason::Brother:
            score.terms[2] = rules.brother_bonus;
            break;
        case HeirReason::Nephew:
            score.terms[2] = rules.nephew_bonus;
            break;
        default:
            break;
    }
    score.opinion_mean = opinion_mean;
    score.terms[3] = weighted_points(opinion_mean * kPointScale, rules.opinion_weight_permille);
    score.terms[4] = weighted_points(t.loyalty_head[i].raw(), rules.loyalty_weight_permille);
    score.terms[5] = weighted_points(t.piety[i].raw(), rules.piety_weight_permille);
    score.terms[6] = (t.legitimacy[i] == Legitimacy::Legitimized ? -rules.legitimized_penalty : 0) +
                     (t.sex[i] == Sex::Female ? -rules.female_penalty : 0);
    score.total = std::accumulate(score.terms.begin(), score.terms.end(), std::int32_t{0});
    score.support =
        opinion_mean + weighted_points(t.loyalty_head[i].raw(), rules.support_loyalty_weight_permille);
    return score;
}

bool base_eligible(const World& world,
                   const SuccessionRules& rules,
                   CharacterId predecessor,
                   CharacterId id) {
    if (id == predecessor || !world.is_alive(id)) {
        return false;
    }
    if (const auto* status = world.family_status(id);
        status != nullptr && (status->flags & family::kPersonStatusFlags) != 0) {
        return false;
    }
    return rules.illegitimate_inherit ||
           world.characters().legitimacy[id.index()] != Legitimacy::Illegitimate;
}

struct LineageOrder {
    const World& world;
    bool youngest_first;
    std::span<const SuccessionCandidate> candidates;
    std::vector<std::size_t> index;
    std::vector<bool> visited;
    std::vector<SuccessionCandidate> rows;

    LineageOrder(const World& source, bool youngest, std::span<const SuccessionCandidate> entries)
        : world(source), youngest_first(youngest), candidates(entries),
          index(source.characters().dynasty.size(), kNotCandidate), visited(index.size()) {
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            index[candidates[i].character.index()] = i;
        }
        rows.reserve(candidates.size());
    }

    bool age_order(CharacterId a, CharacterId b) const {
        const auto& dates = world.characters().birth_date;
        if (dates[a.index()] != dates[b.index()]) {
            return youngest_first ? dates[a.index()] > dates[b.index()] : dates[a.index()] < dates[b.index()];
        }
        return a < b;
    }

    void sort_age(std::vector<CharacterId>& ids) const {
        std::ranges::sort(ids, [&](CharacterId a, CharacterId b) { return age_order(a, b); });
    }

    void append(CharacterId id, HeirReason reason) {
        auto& slot = index[id.index()];
        if (slot == kNotCandidate) {
            return;
        }
        auto candidate = candidates[slot];
        candidate.reason = reason;
        rows.push_back(candidate);
        slot = kNotCandidate;
    }

    void branch(CharacterId root, HeirReason root_reason, HeirReason descendant_reason) {
        std::vector<std::pair<CharacterId, HeirReason>> stack{{root, root_reason}};
        while (!stack.empty()) {
            const auto [id, reason] = stack.back();
            stack.pop_back();
            if (!world.exists(id) || visited[id.index()]) {
                continue;
            }
            visited[id.index()] = true;
            append(id, reason);
            auto children = kin::children_of(world, id);
            sort_age(children);
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                stack.emplace_back(*it, descendant_reason);
            }
        }
    }

    std::vector<SuccessionCandidate> build(const Relations& relations) {
        auto children = kin::children_of(world, relations.predecessor);
        sort_age(children);
        for (const auto& [root_reason, descendant_reason] :
             {std::pair{HeirReason::EldestSon, HeirReason::SonLine},
              std::pair{HeirReason::Daughter, HeirReason::DaughterLine}}) {
            for (const CharacterId id : children) {
                if (relations.classify(id) == root_reason) {
                    const auto reason = youngest_first && root_reason == HeirReason::EldestSon
                                            ? HeirReason::YoungestSon
                                            : root_reason;
                    branch(id, reason, descendant_reason);
                }
            }
        }
        auto siblings = relations.siblings;
        sort_age(siblings);
        for (const CharacterId id : siblings) {
            if (relations.classify(id) == HeirReason::Brother) {
                branch(id, HeirReason::Brother, HeirReason::Nephew);
            }
        }
        std::vector<CharacterId> nephews;
        for (const CharacterId sibling : siblings) {
            for (const CharacterId id : kin::children_of(world, sibling)) {
                if (relations.classify(id) == HeirReason::Nephew) {
                    nephews.push_back(id);
                }
            }
        }
        sort_age(nephews);
        for (const CharacterId id : nephews) {
            branch(id, HeirReason::Nephew, HeirReason::Nephew);
        }
        std::vector<std::pair<std::uint16_t, CharacterId>> remaining;
        for (const auto& row : candidates) {
            if (index[row.character.index()] != kNotCandidate) {
                const auto degree = kin::blood_degree(world, relations.predecessor, row.character);
                remaining.emplace_back(degree == 0 ? std::uint16_t{256} : degree, row.character);
            }
        }
        std::ranges::sort(remaining, [&](const auto& a, const auto& b) {
            return a.first == b.first ? age_order(a.second, b.second) : a.first < b.first;
        });
        for (const auto& [degree, id] : remaining) {
            static_cast<void>(degree);
            append(id, HeirReason::Kin);
        }
        return std::move(rows);
    }
};

bool older_first(const World& world, CharacterId a, CharacterId b) {
    const auto& births = world.characters().birth_date;
    return std::tuple{births[a.index()], a} < std::tuple{births[b.index()], b};
}

void sort_council(const World& world, std::vector<SuccessionCandidate>& rows) {
    std::ranges::sort(rows, [&](const auto& a, const auto& b) {
        return a.council.total == b.council.total ? older_first(world, a.character, b.character)
                                                  : a.council.total > b.council.total;
    });
}

SuccessionDispute propose_dispute(const World& world,
                                  const SuccessionRules& rules,
                                  const SuccessionResult& result,
                                  CharacterId designated,
                                  family::DisputeReason reason) {
    auto ranked = result.order;
    sort_council(world, ranked);
    SuccessionDispute dispute;
    dispute.reason = reason;
    const auto append = [&](const SuccessionCandidate& row) {
        SuccessionClaimant claimant;
        claimant.character = row.character;
        claimant.council_score = row.council.total;
        claimant.lawful_bonus = row.character == result.lawful_heir ? rules.lawful_bonus : 0;
        claimant.appointment_bonus = row.character == designated ? rules.appointment_bonus : 0;
        claimant.score = claimant.council_score + claimant.lawful_bonus + claimant.appointment_bonus;
        dispute.claimants.push_back(claimant);
    };
    for (const auto& row : ranked) {
        if (dispute.claimants.size() == family::kMaxDisputeClaimants ||
            ranked.front().council.total - row.council.total > rules.dispute_margin) {
            break;
        }
        append(row);
    }
    if (std::ranges::none_of(dispute.claimants,
                             [&](const auto& row) { return row.character == result.lawful_heir; })) {
        if (dispute.claimants.size() == family::kMaxDisputeClaimants) {
            dispute.claimants.pop_back();
        }
        const auto lawful = std::ranges::find(ranked, result.lawful_heir, &SuccessionCandidate::character);
        append(*lawful);
    }
    std::ranges::sort(dispute.claimants, [&](const auto& a, const auto& b) {
        return a.score == b.score ? older_first(world, a.character, b.character) : a.score > b.score;
    });
    return dispute;
}

}  // namespace

CouncilScore council_score(const World& world,
                           const SuccessionRules& rules,
                           DynastyId dynasty,
                           CharacterId predecessor,
                           CharacterId candidate,
                           economy::Breakdown* explain) {
    CouncilScore score;
    if (world.dynasty_mode() && world.exists(dynasty) && world.is_alive(candidate) &&
        world.characters().dynasty[candidate.index()] == dynasty) {
        const Relations relations(world, lineage_anchor(world, dynasty, predecessor));
        const CouncilOpinions opinions(world, rules, dynasty);
        score = score_candidate(world, rules, relations, candidate, opinions.mean(world, rules, candidate));
    }
    if (explain != nullptr) {
        explain_council_score(score, *explain);
    }
    return score;
}

SuccessionResult resolve_successor(const World& world,
                                   const SuccessionRules& rules,
                                   DynastyId dynasty,
                                   CharacterId predecessor) {
    if (!world.exists(dynasty)) {
        return {};
    }
    const auto* state = world.family_succession(dynasty);
    return resolve_successor(world,
                             rules,
                             dynasty,
                             predecessor,
                             world.dynasties().succession_law[dynasty.index()],
                             state == nullptr ? CharacterId{} : state->designated_heir);
}

bool succession_candidate_eligible(const World& world,
                                   const SuccessionRules& rules,
                                   DynastyId dynasty,
                                   CharacterId predecessor,
                                   CharacterId candidate) {
    return world.dynasty_mode() && world.exists(dynasty) &&
           base_eligible(world, rules, predecessor, candidate) &&
           world.characters().dynasty[candidate.index()] == dynasty;
}

SuccessionResult resolve_successor(const World& world,
                                   const SuccessionRules& rules,
                                   DynastyId dynasty,
                                   CharacterId predecessor,
                                   SuccessionLaw law,
                                   CharacterId designated) {
    SuccessionResult result;
    if (!world.dynasty_mode() || !world.exists(dynasty) ||
        static_cast<std::uint8_t>(law) >= kSuccessionLawCount ||
        (predecessor.valid() && !world.exists(predecessor))) {
        return result;
    }
    result.predecessor = lineage_anchor(world, dynasty, predecessor);
    const auto& t = world.characters();
    const auto members = world.members_of(dynasty);
    const bool has_adult_male = std::ranges::any_of(members, [&](CharacterId id) {
        return succession_candidate_eligible(world, rules, dynasty, result.predecessor, id) &&
               t.sex[id.index()] == Sex::Male && adult(world, rules, id);
    });
    const Relations relations(world, result.predecessor);
    const CouncilOpinions opinions(world, rules, dynasty);
    for (const CharacterId id : members) {
        if (!succession_candidate_eligible(world, rules, dynasty, result.predecessor, id)) {
            continue;
        }
        const bool female = t.sex[id.index()] == Sex::Female;
        const bool appointed = law == SuccessionLaw::Appointment && id == designated;
        if (female &&
            (rules.female_succession == FemaleSuccession::Never ||
             (rules.female_succession == FemaleSuccession::IfNoAdultMale && has_adult_male && !appointed))) {
            continue;
        }
        result.order.push_back({id,
                                HeirReason::Kin,
                                score_candidate(world, rules, relations, id, opinions.mean(world, rules, id)),
                                !adult(world, rules, id),
                                female});
    }
    if (result.order.empty()) {
        return result;
    }
    if (result.predecessor.valid()) {
        LineageOrder lineage(world, law == SuccessionLaw::Minorat, result.order);
        result.order = lineage.build(relations);
    } else {
        sort_council(world, result.order);
    }
    result.lawful_heir = result.order.front().character;
    CharacterId winner = result.lawful_heir;
    std::optional<family::DisputeReason> dispute_reason;
    if (law == SuccessionLaw::Appointment) {
        const auto chosen = std::ranges::find(result.order, designated, &SuccessionCandidate::character);
        if (chosen == result.order.end()) {
            dispute_reason = family::DisputeReason::NoAppointment;
        } else {
            winner = chosen->character;
            chosen->reason = HeirReason::Appointed;
        }
    } else if (law == SuccessionLaw::FamilyCouncil) {
        sort_council(world, result.order);
        for (auto& row : result.order) {
            row.reason = HeirReason::CouncilChoice;
        }
        winner = result.order.front().character;
        if (result.order.size() > 1 &&
            result.order[0].council.total - result.order[1].council.total < rules.council_clear_margin) {
            dispute_reason = family::DisputeReason::CloseScores;
        }
    } else if (rules.unpopular_dispute_enabled &&
               result.order.front().council.support < rules.dispute_support_threshold) {
        const auto rival = std::find_if(
            result.order.begin() + 1, result.order.end(), [](const auto& row) { return !row.minor; });
        if (rival != result.order.end() && rival->council.total >= result.order.front().council.total) {
            dispute_reason = family::DisputeReason::UnpopularHeir;
        }
    }
    if (dispute_reason) {
        result.kind = SuccessionKind::Dispute;
        result.dispute = propose_dispute(world, rules, result, designated, *dispute_reason);
    } else {
        const auto& selected = *std::ranges::find(result.order, winner, &SuccessionCandidate::character);
        result.kind = SuccessionKind::Successor;
        result.winner = selected.character;
        result.minor = selected.minor;
        result.female_head = selected.female;
        result.honor_delta = selected.female ? rules.female_head_honor : 0;
    }
    return result;
}

void explain_council_score(const CouncilScore& score, economy::Breakdown& explain) {
    explain.begin(succession_term::kCouncilScore, economy::Unit::Points, score.total * kPointScale);
    for (std::size_t i = 0; i < score.terms.size(); ++i) {
        static_cast<void>(explain.add_term(economy::kRootNode,
                                           static_cast<std::uint16_t>(succession_term::kSkills + i),
                                           score.terms[i] * kPointScale));
    }
}

void explain_claimant_score(const SuccessionClaimant& claimant, economy::Breakdown& explain) {
    explain.begin(succession_term::kClaimantScore, economy::Unit::Points, claimant.score * kPointScale);
    static_cast<void>(explain.add_term(
        economy::kRootNode, succession_term::kClaimantCouncil, claimant.council_score * kPointScale));
    static_cast<void>(explain.add_term(
        economy::kRootNode, succession_term::kClaimantLawful, claimant.lawful_bonus * kPointScale));
    static_cast<void>(explain.add_term(
        economy::kRootNode, succession_term::kClaimantAppointment, claimant.appointment_bonus * kPointScale));
}

std::string_view succession_term_key(std::uint16_t code) noexcept {
    return economy::find_term_key(kSuccessionTermKeys, code);
}

SuccessionResult
LawSuccessionPolicy::resolve(const World& world, DynastyId dynasty, CharacterId predecessor) const {
    return resolve_successor(world, rules_, dynasty, predecessor);
}

}  // namespace pat::sim
