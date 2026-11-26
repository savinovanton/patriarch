#pragma once

#include <array>
#include <cstdint>

namespace pat::sim {

enum class FemaleSuccession : std::uint8_t { Never, IfNoAdultMale, Always };

/// Content, not World state. Defaults follow marriage-succession-model sections 8.5 and 17.3;
/// E8-T19 loads the same fields from data. Weights are per-mille, scores are whole points.
struct SuccessionRules {
    // Indexed by the stable SuccessionLaw wire value; the council and minorat remain scenario laws.
    std::array<bool, 5> reform_selectable{true, false, true, false, true};
    std::int32_t reform_cooldown_days = 1825;
    std::int64_t reform_money_kop = 30000;
    std::int32_t reform_honor = 25;
    std::int32_t reform_unity = 10;
    std::int32_t reform_first_opinion = 25;
    std::int32_t reform_other_opinion = 5;
    std::uint8_t reform_max_losers = 8;
    std::int32_t appointment_skip_unity = 3;
    std::int32_t appointment_skip_opinion = 15;
    std::int32_t disown_unity = 5;
    std::int32_t disown_opinion = 60;
    std::int32_t disown_loyalty = 30;

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
