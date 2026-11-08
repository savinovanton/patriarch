#pragma once

#include "pat/sim/world.hpp"

#include <cstddef>
#include <limits>
#include <vector>

// Read-only queries over the family tree (E7-T03, ADR-0006 D2). They use the parent columns and the
// derived child lists of World, so they are available in dynasty mode; in E1 mode (where the ids of
// the dead are reused and no list is kept) they return nothing.
//
// Every result is in a documented, deterministic order that depends only on the state of the world,
// never on the order in which people were created or the lists were built:
//   * "eldest first" = ascending (birth_date, id);
//   * generation order = nearest generation first; inside a generation the order in which the
//     previous generation was visited, father before mother for ancestors, eldest child first for
//     descendants. A person reached by several paths is listed once, at the first visit.
// Ids that do not exist (invalid, out of range, free slot) give an empty result. Archived people
// (E7-T04) are ordinary members of the tree.

namespace pat::sim::kin {

inline constexpr std::size_t kAllGenerations = std::numeric_limits<std::size_t>::max();

/// Children of `parent` (as father or as mother), eldest first.
[[nodiscard]] std::vector<CharacterId> children_of(const World& world, CharacterId parent);

/// Number of children of `parent`, without building the list.
[[nodiscard]] std::size_t child_count(const World& world, CharacterId parent) noexcept;

/// Full and half siblings of `who` (everybody who shares the father or the mother), eldest first,
/// `who` excluded, each person once.
[[nodiscard]] std::vector<CharacterId> siblings_of(const World& world, CharacterId who);

/// Ancestors of `who` in generation order, at most `max_generations` generations up (1 = the
/// parents). `who` itself is not included.
[[nodiscard]] std::vector<CharacterId>
ancestors_of(const World& world, CharacterId who, std::size_t max_generations = kAllGenerations);

/// Descendants of `who` in generation order, at most `max_generations` generations down (1 = the
/// children). `who` itself is not included.
[[nodiscard]] std::vector<CharacterId>
descendants_of(const World& world, CharacterId who, std::size_t max_generations = kAllGenerations);

/// True when `ancestor` is a (direct) ancestor of `person`.
[[nodiscard]] bool is_ancestor_of(const World& world, CharacterId ancestor, CharacterId person);

/// The branch a newborn of this couple belongs to by default: the father's branch when the child
/// stays in the father's dynasty, otherwise the mother's when it stays in hers, otherwise none.
/// (Used by births, E7-T12; legitimacy of the birth stays an input of the creator, see ADR-0006 D6.)
[[nodiscard]] BranchId
inherited_branch(const World& world, CharacterId father, CharacterId mother, DynastyId child_dynasty);

}  // namespace pat::sim::kin
