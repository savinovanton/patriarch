#pragma once

#include "pat/sim/command.hpp"
#include "pat/sim/marriage_check.hpp"

#include <optional>

namespace pat::sim {

inline constexpr StandingReason kStandingReasonThirdMarriage = 102;
inline constexpr StandingReason kStandingReasonBetrothalBroken = 103;
inline constexpr StandingReason kStandingReasonWeddingRank = 105;
inline constexpr OpinionReason kOpinionReasonBetrothalBroken = 103;
inline constexpr OpinionReason kOpinionReasonMarriedInto = 105;

[[nodiscard]] core::Result<void, CommandError> validate_betroth(const World& world,
                                                                const MarriageRules& rules,
                                                                const cmd::Betroth& payload,
                                                                const FaithPolicy* faith = nullptr);
[[nodiscard]] core::Result<void, CommandError> validate_break_betrothal(const World& world,
                                                                        const MarriageRules& rules,
                                                                        DynastyId actor,
                                                                        const cmd::BreakBetrothal& payload,
                                                                        const FaithPolicy* faith = nullptr);
/// A wedding queued on its deadline remains valid at the next tick (deadline + one day).
[[nodiscard]] core::Result<void, CommandError> validate_wed(const World& world,
                                                            const MarriageRules& rules,
                                                            const cmd::Wed& payload,
                                                            const FaithPolicy* faith = nullptr);
[[nodiscard]] core::Result<void, CommandError> validate_settle_widowhood(const World& world,
                                                                         const cmd::SettleWidowhood& payload);

void apply_betroth(World& world, const MarriageRules& rules, const cmd::Betroth& payload);
void apply_break_betrothal(World& world,
                           const MarriageRules& rules,
                           DynastyId actor,
                           const cmd::BreakBetrothal& payload);
void apply_wed(World& world, const MarriageRules& rules, const cmd::Wed& payload);
void apply_settle_widowhood(World& world, const cmd::SettleWidowhood& payload);

/// Pure daily decision; age alone does not break a betrothal before its wedding date.
[[nodiscard]] std::optional<family::BetrothalBreakReason>
wedding_break_reason(const World& world,
                     const MarriageRules& rules,
                     const family::Betrothal& row,
                     const FaithPolicy* faith = nullptr);

}  // namespace pat::sim
