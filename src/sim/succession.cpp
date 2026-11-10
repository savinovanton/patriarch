#include "pat/sim/succession.hpp"

#include "pat/sim/demography.hpp"
#include "pat/sim/kinship.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <tuple>

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
        return world.is_alive(id) &&
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

}  // namespace pat::sim
