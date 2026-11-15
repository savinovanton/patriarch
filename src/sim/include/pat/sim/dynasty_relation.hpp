#pragma once

#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/marriage_rules.hpp"
#include "pat/sim/world.hpp"

#include <cstdint>

namespace pat::sim {

/// Symmetric relation of two houses, -100..100: mean of stored nonzero opinions of living
/// members (excluding the two heads' opinions of one another), plus current living marriage ties.
/// Pure; unknown or identical houses and worlds outside dynasty mode return zero. The optional
/// additive explanation uses marriage_term::kRelation and Fixed32 raw Points.
[[nodiscard]] std::int32_t dynasty_relation(const World& world,
                                            DynastyId a,
                                            DynastyId b,
                                            const MarriageRules& rules = MarriageRules::defaults(),
                                            economy::Breakdown* explain = nullptr);

}  // namespace pat::sim
