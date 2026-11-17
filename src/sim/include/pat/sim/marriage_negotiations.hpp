#pragma once

#include "pat/sim/command.hpp"
#include "pat/sim/marriage_check.hpp"

#include <optional>

namespace pat::sim {

inline constexpr StandingReason kStandingReasonEnvoyDispensation = 101;
inline constexpr OpinionReason kOpinionReasonMarriageRefused = 101;
inline constexpr OpinionReason kOpinionReasonMarriageAccepted = 102;

/// Pure answer calculation from a pending row and this snapshot; never draws random numbers.
/// A changed house cancels immediately, including retained snapshots from older saves. Other
/// answers are calculated only when due. No value means there is no response to emit yet.
[[nodiscard]] std::optional<cmd::RespondMarriage>
compute_marriage_response(const World& world,
                          const MarriageRules& rules,
                          family::NegotiationId negotiation,
                          const FaithPolicy* faith = nullptr);

/// Validates a system answer before any family row, opinion or chronicle entry changes.
[[nodiscard]] core::Result<void, CommandError>
validate_marriage_response(const World& world,
                           const MarriageRules& rules,
                           const cmd::RespondMarriage& response,
                           const FaithPolicy* faith = nullptr);

/// Applies an answer validated against the same World; dowry is retained until the wedding.
void respond_marriage(World& world,
                      const MarriageRules& rules,
                      const cmd::RespondMarriage& response,
                      const FaithPolicy* faith = nullptr);

}  // namespace pat::sim
