#pragma once

#include "pat/sim/command.hpp"

#include <array>
#include <vector>

namespace pat::sim {

inline constexpr OpinionReason kOpinionReasonGodparent = 106;

struct GodparentCandidate {
    CharacterId character;
    std::int32_t score = 0;
    [[nodiscard]] bool operator==(const GodparentCandidate&) const = default;
};

/// A living child with no assigned godparents, aged zero through max_age_days inclusive.
[[nodiscard]] bool awaits_godparents(const World& world, CharacterId child, std::int32_t max_age_days = 1);

/// Living adults in the parents' houses, excluding the child, parents and full/half siblings.
/// Ordered by descending score and ascending id. No randomness or persistent cache is used.
[[nodiscard]] std::vector<GodparentCandidate> godparent_candidates(const World& world, CharacterId child);

/// Prefer the best man and woman when both are available; otherwise take the best two.
/// The result retains score/id order and can contain zero or one candidate.
[[nodiscard]] std::array<CharacterId, kGodparentSlots> choose_godparents(const World& world,
                                                                         CharacterId child);

[[nodiscard]] core::Result<void, CommandError>
validate_assign_godparents(const World& world, const cmd::AssignGodparents& payload);
void apply_assign_godparents(World& world, const cmd::AssignGodparents& payload);

}  // namespace pat::sim
