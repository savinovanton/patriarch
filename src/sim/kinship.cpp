#include "pat/sim/kinship.hpp"

#include <algorithm>
#include <utility>

namespace pat::sim::kin {

namespace {

/// Head of the list a parent keeps and the link array to follow: a father walks `next_by_father`, a
/// mother `next_by_mother`.
[[nodiscard]] const std::vector<CharacterId>& next_links(const CharacterTable& t, CharacterId parent) {
    return t.sex[parent.index()] == Sex::Male ? t.next_by_father : t.next_by_mother;
}

template <class Visit>
void for_each_child(const World& world, CharacterId parent, Visit&& visit) {
    if (!world.exists(parent)) {
        return;
    }
    const CharacterTable& t = world.characters();
    const std::vector<CharacterId>& next = next_links(t, parent);
    // check_invariants() guarantees the lists are finite; the bound makes a damaged world safe anyway.
    std::size_t steps = 0;
    for (CharacterId c = t.first_child[parent.index()];
         c.valid() && c.index() < next.size() && steps <= next.size();
         c = next[c.index()], ++steps) {
        visit(c);
    }
}

[[nodiscard]] bool born_before(const CharacterTable& t, CharacterId a, CharacterId b) noexcept {
    const Date da = t.birth_date[a.index()];
    const Date db = t.birth_date[b.index()];
    return da < db || (da == db && a < b);
}

struct AncestorDistance {
    CharacterId character;
    std::uint8_t depth = 0;
};

/// Breadth-first traversal records the shortest visit to each reachable ancestor. The list has at
/// most 127 entries (self and six binary generations), independent of the size of World.
std::vector<AncestorDistance>
ancestor_distances(const World& world, CharacterId person, std::size_t max_generations) {
    const auto limit = std::min<std::size_t>(max_generations, 6);
    std::vector<AncestorDistance> out;
    out.reserve(127);
    out.push_back({person, 0});
    const auto& people = world.characters();
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto current = out[i];
        if (current.depth >= limit) {
            continue;
        }
        for (const CharacterId parent :
             {people.father[current.character.index()], people.mother[current.character.index()]}) {
            if (!world.exists(parent) ||
                std::ranges::find(out, parent, &AncestorDistance::character) != out.end()) {
                continue;
            }
            out.push_back({parent, static_cast<std::uint8_t>(current.depth + 1)});
        }
    }
    return out;
}

bool is_godparent(const World& world, CharacterId godparent, CharacterId child) {
    if (!world.exists(godparent) || !world.exists(child)) {
        return false;
    }
    const auto& godparents = world.characters().godparent[child.index()];
    return std::ranges::find(godparents, godparent) != godparents.end();
}

bool godparent_of_child(const World& world, CharacterId godparent, CharacterId parent) {
    bool related = false;
    for_each_child(world, parent, [&](CharacterId child) {
        related = related || is_godparent(world, godparent, child);
    });
    return related;
}

}  // namespace

std::vector<CharacterId> children_of(const World& world, CharacterId parent) {
    std::vector<CharacterId> out;
    for_each_child(world, parent, [&](CharacterId c) { out.push_back(c); });
    return out;
}

std::size_t child_count(const World& world, CharacterId parent) noexcept {
    std::size_t n = 0;
    for_each_child(world, parent, [&](CharacterId) { ++n; });
    return n;
}

std::vector<CharacterId> siblings_of(const World& world, CharacterId who) {
    std::vector<CharacterId> out;
    if (!world.dynasty_mode() || !world.exists(who)) {
        return out;
    }
    const CharacterTable& t = world.characters();
    for (const CharacterId parent : {t.father[who.index()], t.mother[who.index()]}) {
        for_each_child(world, parent, [&](CharacterId c) {
            if (c != who) {
                out.push_back(c);
            }
        });
    }
    std::sort(out.begin(), out.end(), [&](CharacterId a, CharacterId b) { return born_before(t, a, b); });
    out.erase(std::unique(out.begin(), out.end()), out.end());  // full siblings appear in both lists
    return out;
}

std::vector<CharacterId> ancestors_of(const World& world, CharacterId who, std::size_t max_generations) {
    std::vector<CharacterId> out;
    if (!world.dynasty_mode() || !world.exists(who)) {
        return out;
    }
    const CharacterTable& t = world.characters();
    std::vector<std::uint8_t> seen(t.ids.capacity(), 0);
    seen[who.index()] = 1;
    std::vector<CharacterId> level{who};
    std::vector<CharacterId> next_level;
    for (std::size_t generation = 0; generation < max_generations && !level.empty(); ++generation) {
        next_level.clear();
        for (const CharacterId person : level) {
            for (const CharacterId parent : {t.father[person.index()], t.mother[person.index()]}) {
                if (world.exists(parent) && seen[parent.index()] == 0) {
                    seen[parent.index()] = 1;
                    next_level.push_back(parent);
                    out.push_back(parent);
                }
            }
        }
        level.swap(next_level);
    }
    return out;
}

std::vector<CharacterId> descendants_of(const World& world, CharacterId who, std::size_t max_generations) {
    std::vector<CharacterId> out;
    if (!world.exists(who)) {
        return out;
    }
    std::vector<std::uint8_t> seen(world.characters().ids.capacity(), 0);
    seen[who.index()] = 1;
    std::vector<CharacterId> level{who};
    std::vector<CharacterId> next_level;
    for (std::size_t generation = 0; generation < max_generations && !level.empty(); ++generation) {
        next_level.clear();
        for (const CharacterId person : level) {
            for_each_child(world, person, [&](CharacterId c) {
                if (seen[c.index()] == 0) {
                    seen[c.index()] = 1;
                    next_level.push_back(c);
                    out.push_back(c);
                }
            });
        }
        level.swap(next_level);
    }
    return out;
}

bool is_ancestor_of(const World& world, CharacterId ancestor, CharacterId person) {
    if (!world.exists(ancestor) || !world.exists(person) || ancestor == person) {
        return false;
    }
    const std::vector<CharacterId> all = ancestors_of(world, person);
    return std::find(all.begin(), all.end(), ancestor) != all.end();
}

std::uint8_t blood_degree(const World& world, CharacterId a, CharacterId b, std::size_t max_generations) {
    if (!world.dynasty_mode() || !world.exists(a) || !world.exists(b) || a == b) {
        return 0;
    }
    const auto ancestors_a = ancestor_distances(world, a, max_generations);
    const auto ancestors_b = ancestor_distances(world, b, max_generations);
    std::uint8_t degree = 0;
    for (const auto& left : ancestors_a) {
        for (const auto& right : ancestors_b) {
            if (left.character == right.character) {
                const auto distance = static_cast<std::uint8_t>(left.depth + right.depth);
                if (degree == 0 || distance < degree) {
                    degree = distance;
                }
            }
        }
    }
    return degree;
}

std::uint8_t affinity_degree(const World& world, CharacterId a, CharacterId b, std::size_t max_generations) {
    if (!world.dynasty_mode() || !world.exists(a) || !world.exists(b) || a == b) {
        return 0;
    }
    std::uint8_t degree = 0;
    const auto& people = world.characters();
    const auto consider = [&](CharacterId spouse, CharacterId person) {
        if (!world.exists(spouse)) {
            return;
        }
        const auto distance = blood_degree(world, spouse, person, max_generations);
        if (distance != 0 && (degree == 0 || distance < degree)) {
            degree = distance;
        }
    };
    for (const auto& [person, other] : {std::pair{a, b}, std::pair{b, a}}) {
        consider(people.spouse[person.index()], other);
        for (const CharacterId spouse : people.ex_spouses[person.index()]) {
            consider(spouse, other);
        }
    }
    return degree;
}

bool spiritually_related(const World& world, CharacterId a, CharacterId b) {
    if (!world.dynasty_mode() || !world.exists(a) || !world.exists(b) || a == b) {
        return false;
    }
    if (is_godparent(world, a, b) || is_godparent(world, b, a) || godparent_of_child(world, a, b) ||
        godparent_of_child(world, b, a)) {
        return true;
    }
    const auto& people = world.characters();
    for (const CharacterId left : people.godparent[a.index()]) {
        if (!world.exists(left)) {
            continue;
        }
        for (const CharacterId right : people.godparent[b.index()]) {
            if (world.exists(right) && left == right) {
                return true;
            }
        }
    }
    return false;
}

BranchId
inherited_branch(const World& world, CharacterId father, CharacterId mother, DynastyId child_dynasty) {
    const CharacterTable& t = world.characters();
    for (const CharacterId parent : {father, mother}) {
        if (world.exists(parent) && t.dynasty[parent.index()] == child_dynasty) {
            const BranchId branch = t.branch[parent.index()];
            if (branch.valid()) {
                return branch;
            }
        }
    }
    return BranchId();
}

}  // namespace pat::sim::kin
