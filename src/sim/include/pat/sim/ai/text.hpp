#pragma once

#include "pat/sim/ai/rules.hpp"

#include <cstdint>
#include <string_view>

namespace pat::sim::ai {

// Shared presentation metadata for AI settings and developer snapshots.
inline constexpr std::string_view kTitleKey = "ui.ai.title";
inline constexpr std::string_view kDifficultyKey = "ui.ai.difficulty";
inline constexpr std::string_view kApKey = "ui.ai.ap";
inline constexpr std::string_view kWuKey = "ui.ai.wu";
inline constexpr std::string_view kQueueKey = "ui.ai.queue";
inline constexpr std::string_view kFrameKey = "ui.ai.frame";
inline constexpr std::string_view kLogKey = "ui.ai.log";
inline constexpr std::string_view kGoalKey = "ui.ai.goal";
inline constexpr std::string_view kArchetypeKey = "ui.ai.archetype";
inline constexpr std::string_view kTierKey = "ui.ai.tier";
inline constexpr std::string_view kDueKey = "ui.ai.due";
inline constexpr std::string_view kCommitmentKey = "ui.ai.commitment";
inline constexpr std::string_view kCooldownKey = "ui.ai.cooldown";
inline constexpr std::string_view kDevOnlyKey = "ui.ai.dev_only";
inline constexpr std::string_view kFactorsKey = "ui.ai.factors";

// Unknown enumeration values and reason codes return an empty view.
[[nodiscard]] std::string_view goal_key(GoalKind value) noexcept;
[[nodiscard]] std::string_view archetype_key(Archetype value) noexcept;
[[nodiscard]] std::string_view difficulty_key(Difficulty value) noexcept;
// Stable common AiReason codes 1...16, ai-model.md section 11.2.
// Domain-specific reason blocks are added with their planners.
[[nodiscard]] std::string_view reason_key(std::uint16_t code) noexcept;

}  // namespace pat::sim::ai
