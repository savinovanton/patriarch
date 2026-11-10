#pragma once

#include "pat/sim/entity_ids.hpp"

namespace pat::sim {

class World;

/// E7's fallback for a vacant house after its head dies. E8 supplies the succession laws.
/// Returns an invalid id if the house has no eligible adult member.
[[nodiscard]] CharacterId fallback_successor(const World& world, DynastyId dynasty, CharacterId predecessor);

}  // namespace pat::sim
