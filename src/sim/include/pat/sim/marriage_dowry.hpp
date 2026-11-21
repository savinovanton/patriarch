#pragma once

#include "pat/sim/economy/money.hpp"
#include "pat/sim/family/tables.hpp"

namespace pat::sim {

class World;
struct MarriageRules;

/// Returns the newly paid part of the configured refund and records it on the active marriage.
/// Call before ending the marriage. Repeated calls cap total payments at the configured share.
[[nodiscard]] economy::Kopecks
refund_marriage_dowry(World& world, const MarriageRules& rules, family::MarriageId marriage);

}  // namespace pat::sim
