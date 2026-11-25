#include "pat/sim/marriage_godparents.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/kinship.hpp"

#include <algorithm>
#include <cstdlib>
#include <tuple>

namespace pat::sim {
namespace {

using Godparents = std::array<CharacterId, kGodparentSlots>;

bool sibling(const World& world, CharacterId a, CharacterId b) {
    const auto siblings = kin::siblings_of(world, a);
    return std::ranges::find(siblings, b) != siblings.end();
}

bool eligible(const World& world, CharacterId child, CharacterId candidate) {
    if (!world.is_alive(candidate) || child == candidate) {
        return false;
    }
    const auto& people = world.characters();
    const auto father = people.father[child.index()];
    const auto mother = people.mother[child.index()];
    if (candidate == father || candidate == mother || sibling(world, child, candidate) ||
        age_years(people.birth_date[candidate.index()], world.today()) < 16) {
        return false;
    }
    const auto house = people.dynasty[candidate.index()];
    return (world.exists(father) && house == people.dynasty[father.index()]) ||
           (world.exists(mother) && house == people.dynasty[mother.index()]);
}

std::int32_t candidate_score(const World& world, CharacterId child, CharacterId candidate) {
    const auto& people = world.characters();
    const auto father = people.father[child.index()];
    const auto mother = people.mother[child.index()];
    std::int32_t score =
        world.exists(father) && people.dynasty[candidate.index()] == people.dynasty[father.index()] ? 20 : 10;
    std::int64_t opinion = 0;
    std::int32_t parents = 0;
    bool aunt_or_uncle = false;
    for (const auto parent : {father, mother}) {
        if (world.exists(parent)) {
            opinion += world.get_opinion(parent, candidate).raw();
            ++parents;
            aunt_or_uncle = aunt_or_uncle || sibling(world, parent, candidate);
        }
    }
    if (parents != 0) {
        const auto scale = core::Fixed32::from_int(1).raw();
        const auto mean = std::clamp<std::int64_t>(opinion / parents, -50LL * scale, 50LL * scale);
        score += static_cast<std::int32_t>(mean * 300 / (1000LL * scale));
    }
    score += aunt_or_uncle ? 10 : 0;
    score -= 5 * (std::abs(age_years(people.birth_date[candidate.index()], world.today()) - 35) / 10);
    return score;
}

}  // namespace

bool awaits_godparents(const World& world, CharacterId child, std::int32_t max_age_days) {
    if (!world.is_alive(child)) {
        return false;
    }
    const auto& people = world.characters();
    const auto age_days = world.today().days() - people.birth_date[child.index()].days();
    return age_days >= 0 && age_days <= max_age_days && people.godparent[child.index()] == Godparents{};
}

std::vector<GodparentCandidate> godparent_candidates(const World& world, CharacterId child) {
    std::vector<GodparentCandidate> candidates;
    if (!world.dynasty_mode() || !world.is_alive(child)) {
        return candidates;
    }
    const auto& people = world.characters();
    DynastyId previous_house;
    for (const auto parent : {people.father[child.index()], people.mother[child.index()]}) {
        if (!world.exists(parent)) {
            continue;
        }
        const auto house = people.dynasty[parent.index()];
        if (house == previous_house) {
            continue;
        }
        previous_house = house;
        for (const auto candidate : world.members_of(house)) {
            if (eligible(world, child, candidate)) {
                candidates.push_back({candidate, candidate_score(world, child, candidate)});
            }
        }
    }
    std::ranges::sort(candidates, [](const auto& a, const auto& b) {
        return std::tuple{-a.score, a.character} < std::tuple{-b.score, b.character};
    });
    return candidates;
}

std::array<CharacterId, kGodparentSlots> choose_godparents(const World& world, CharacterId child) {
    const auto candidates = godparent_candidates(world, child);
    Godparents result{};
    if (candidates.empty()) {
        return result;
    }
    result[0] = candidates.front().character;
    const auto sex = world.characters().sex[result[0].index()];
    const auto other = std::ranges::find_if(
        candidates, [&](const auto& row) { return world.characters().sex[row.character.index()] != sex; });
    if (other != candidates.end()) {
        result[1] = other->character;
    } else if (candidates.size() > 1) {
        result[1] = candidates[1].character;
    }
    return result;
}

core::Result<void, CommandError> validate_assign_godparents(const World& world,
                                                            const cmd::AssignGodparents& payload) {
    using Check = core::Result<void, CommandError>;
    if (!world.dynasty_mode()) {
        return Check::err({Rejection::InvalidState, "godparents require dynasty mode"});
    }
    if (!world.exists(payload.child)) {
        return Check::err({Rejection::UnknownEntity, "unknown child or scan endpoint"});
    }
    if (payload.child.index() < world.family().godparent_cursor) {
        return Check::err({Rejection::InvalidState, "the godparent cursor already passed this row"});
    }
    // A prior queued assignment can fail after a death or membership change. Later commands must
    // leave that newborn available for the next run instead of acknowledging it accidentally.
    for (auto row = world.family().godparent_cursor; row < payload.child.index(); ++row) {
        const CharacterId earlier(row);
        if (awaits_godparents(world, earlier)) {
            return Check::err({Rejection::InvalidState, "an earlier newborn still awaits assignment"});
        }
    }
    if (payload.godparents == Godparents{}) {
        return Check::ok();
    }
    if (!awaits_godparents(world, payload.child, 2)) {
        return Check::err({Rejection::InvalidState, "the child is not an unassigned newborn"});
    }
    if (!payload.godparents[0].valid() || payload.godparents[0] == payload.godparents[1]) {
        return Check::err({Rejection::InvalidArgument, "godparents must be distinct and packed"});
    }
    for (const auto candidate : payload.godparents) {
        if (candidate.valid() && !eligible(world, payload.child, candidate)) {
            return Check::err({Rejection::InvalidArgument, "ineligible godparent"});
        }
    }
    return Check::ok();
}

void apply_assign_godparents(World& world, const cmd::AssignGodparents& payload) {
    if (payload.godparents != Godparents{}) {
        PAT_VERIFY(world.set_character_godparents(payload.child, payload.godparents));
        const auto& people = world.characters();
        for (const auto godparent : payload.godparents) {
            if (!godparent.valid()) {
                continue;
            }
            for (const auto parent :
                 {people.father[payload.child.index()], people.mother[payload.child.index()]}) {
                if (world.is_alive(parent)) {
                    PAT_VERIFY(world.add_opinion(
                        godparent, parent, core::Fixed32::from_int(5), kOpinionReasonGodparent));
                    PAT_VERIFY(world.add_opinion(
                        parent, godparent, core::Fixed32::from_int(5), kOpinionReasonGodparent));
                }
            }
        }
    }
    PAT_VERIFY(world.set_family_godparent_cursor(static_cast<std::uint32_t>(payload.child.index() + 1)));
}

}  // namespace pat::sim
