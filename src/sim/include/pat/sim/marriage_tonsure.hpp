#pragma once

#include "pat/sim/command.hpp"
#include "pat/sim/marriage_rules.hpp"

namespace pat::sim {

inline constexpr StandingReason kStandingReasonTonsure = 104;
inline constexpr OpinionReason kOpinionReasonTonsuredSpouse = 104;

/// Read-only preflight. The actor must control a current member of the married pair, or the target.
[[nodiscard]] core::Result<void, CommandError>
validate_tonsure(const World& world, DynastyId actor, const cmd::Tonsure& payload);
void apply_tonsure(World& world, const MarriageRules& rules, DynastyId actor, const cmd::Tonsure& payload);

}  // namespace pat::sim
