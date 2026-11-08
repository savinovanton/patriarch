#pragma once

#include "pat/core/ids.hpp"

namespace pat::sim {

// Typed ids of the entities that live in the World. They sit in their own header (not in world.hpp)
// so that table definitions which World owns, such as the economy tables, can name them without a
// circular include.
using CharacterId = core::StrongId<struct CharacterIdTag>;
using ProvinceId = core::StrongId<struct ProvinceIdTag>;
using DynastyId = core::StrongId<struct DynastyIdTag>;
using BranchId = core::StrongId<struct BranchIdTag>;  // a branch (sub-house) inside a dynasty

}  // namespace pat::sim
