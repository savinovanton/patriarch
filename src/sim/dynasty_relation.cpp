#include "pat/sim/dynasty_relation.hpp"

#include "pat/sim/marriage_score.hpp"

#include <algorithm>
#include <cstdint>

namespace pat::sim {
namespace {

void explain_relation(std::int32_t mean, std::int32_t ties, std::int32_t total, economy::Breakdown* explain) {
    if (explain == nullptr) {
        return;
    }
    constexpr std::int64_t scale = core::Fixed32::one_raw;
    explain->begin(marriage_term::kRelation, economy::Unit::Points, total * scale);
    static_cast<void>(
        explain->add_term(economy::kRootNode, marriage_term::kRelationOpinionMean, mean * scale));
    static_cast<void>(
        explain->add_term(economy::kRootNode, marriage_term::kRelationMarriageTies, ties * scale));
    if (total != mean + ties) {
        static_cast<void>(explain->add_term(
            economy::kRootNode, marriage_term::kRelationLimit, (total - mean - ties) * scale));
    }
}

}  // namespace

std::int32_t dynasty_relation(
    const World& world, DynastyId a, DynastyId b, const MarriageRules& rules, economy::Breakdown* explain) {
    if (!world.dynasty_mode() || a == b || !world.exists(a) || !world.exists(b)) {
        explain_relation(0, 0, 0, explain);
        return 0;
    }

    const auto& characters = world.characters();
    const CharacterId head_a = world.dynasties().head[a.index()];
    const CharacterId head_b = world.dynasties().head[b.index()];
    std::int64_t sum_raw = 0;
    std::int64_t count = 0;
    const auto collect = [&](DynastyId from, DynastyId to, CharacterId head, CharacterId other_head) {
        for (const CharacterId holder : world.members_of(from)) {
            for (const auto& opinion : world.opinions_from(holder)) {
                if (!world.is_alive(opinion.target) || characters.dynasty[opinion.target.index()] != to ||
                    (holder == head && opinion.target == other_head)) {
                    continue;
                }
                const auto raw = opinion.total().raw();
                if (raw != 0) {
                    sum_raw += raw;
                    ++count;
                }
            }
        }
    };
    collect(a, b, head_a, head_b);
    collect(b, a, head_b, head_a);
    const auto mean = count == 0 ? 0 : static_cast<std::int32_t>(sum_raw / (count * core::Fixed32::one_raw));

    std::int32_t marriages = 0;
    // Each current pair has exactly one member in a, so its reciprocal link is never counted twice.
    for (const CharacterId member : world.members_of(a)) {
        const CharacterId spouse = characters.spouse[member.index()];
        if (world.is_alive(spouse) && characters.dynasty[spouse.index()] == b) {
            if (marriages < rules.relation.max_marriage_ties) {
                ++marriages;
            }
        }
    }
    const std::int32_t ties = marriages * rules.relation.marriage_tie_bonus;
    const std::int32_t total = std::clamp(mean + ties, -100, 100);
    explain_relation(mean, ties, total, explain);
    return total;
}

}  // namespace pat::sim
