#pragma once

#include <cstdint>

namespace pat::sim {

enum class FemaleSuccession : std::uint8_t { Never, IfNoAdultMale, Always };

/// Content, not World state. Defaults follow marriage-succession-model sections 8.5 and 17.3;
/// E8-T19 loads the same fields from data. Weights are per-mille, scores are whole points.
struct SuccessionRules {
    FemaleSuccession female_succession = FemaleSuccession::IfNoAdultMale;
    bool illegitimate_inherit = false;
    std::int32_t adult_age = 16;
    std::int32_t female_head_honor = -10;

    std::int32_t skills_weight_permille = 400;
    std::int32_t age_weight_permille = 500;
    std::int32_t age_years_cap = 34;
    std::int32_t son_bonus = 30;
    std::int32_t daughter_bonus = 15;
    std::int32_t brother_bonus = 20;
    std::int32_t nephew_bonus = 10;
    std::int32_t opinion_weight_permille = 500;
    std::int32_t loyalty_weight_permille = 200;
    std::int32_t piety_weight_permille = 100;
    std::int32_t legitimized_penalty = 10;
    std::int32_t female_penalty = 15;
    std::int32_t council_clear_margin = 15;

    bool unpopular_dispute_enabled = true;
    std::int32_t support_loyalty_weight_permille = 500;
    std::int32_t dispute_support_threshold = -20;
    std::int32_t dispute_margin = 20;
    std::int32_t lawful_bonus = 10;
    std::int32_t appointment_bonus = 20;

    [[nodiscard]] static SuccessionRules defaults() noexcept { return {}; }
    [[nodiscard]] bool operator==(const SuccessionRules&) const = default;
};

}  // namespace pat::sim
