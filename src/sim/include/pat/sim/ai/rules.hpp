#pragma once
#include "pat/core/result.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pat::sim::ai {
// Stable M1 order, docs/dev/ai-model.md sections 3 and 8. No World state is installed here.
enum class GoalKind : std::uint8_t {
    GainOffice = 1,
    SecureLine,
    GrowWealth,
    GainFavor,
    ProtectSelf,
    PreparePlot,
    KeepOrder
};
enum class Archetype : std::uint8_t {
    Ambitious = 1,
    Loyalist,
    Steward,
    Schemer,
    Acquisitive,
    Pious,
    Warrior,
    Reformer
};
enum class Difficulty : std::uint8_t { Friendly = 1, Realistic, Ruthless };
inline constexpr std::array<std::string_view, 7> kGoalKeys{
    "gain_office", "secure_line", "grow_wealth", "gain_favor", "protect_self", "prepare_plot", "keep_order"};
inline constexpr std::array<std::string_view, 8> kArchetypeKeys{
    "ambitious", "loyalist", "steward", "schemer", "acquisitive", "pious", "warrior", "reformer"};
inline constexpr std::array<std::string_view, 3> kDifficultyKeys{"friendly", "realistic", "ruthless"};
#define PAT_AI_BUDGET_FIELDS(X)                                                                              \
    X(quota_wu, 12000, 1, 1000000)                                                                           \
    X(carry_pm, 500, 0, 500)                                                                                 \
    X(pass_wu, 50, 1, 10000)                                                                                 \
    X(goal_wu, 20, 1, 10000)                                                                                 \
    X(filter_rows, 8, 1, 10000)                                                                              \
    X(filter_wu, 1, 1, 10000)                                                                                \
    X(object_wu, 2, 1, 10000)                                                                                \
    X(marriage_wu, 30, 1, 10000)                                                                             \
    X(office_wu, 20, 1, 10000)                                                                               \
    X(plot_wu, 20, 1, 10000)                                                                                 \
    X(construction_wu, 15, 1, 10000)                                                                         \
    X(tax_wu, 10, 1, 10000)                                                                                  \
    X(loan_wu, 10, 1, 10000)                                                                                 \
    X(battle_wu, 25, 1, 10000)                                                                               \
    X(emit_wu, 10, 1, 10000)                                                                                 \
    X(log_wu, 2, 1, 10000)
struct BudgetRules {
#define PAT_AI_DECLARE(name, def, lo, hi) std::int32_t name = 0;
    PAT_AI_BUDGET_FIELDS(PAT_AI_DECLARE)
#undef PAT_AI_DECLARE
    bool operator==(const BudgetRules&) const = default;
};
#define PAT_AI_SCHEDULE_FIELDS(X)                                                                            \
    X(tier1_days, 7, 1, 365)                                                                                 \
    X(tier2_days, 30, 1, 365)                                                                                \
    X(tier3_days, 90, 1, 365)                                                                                \
    X(tier1_threshold, 40, 0, 10000)                                                                         \
    X(tier2_threshold, 15, 0, 10000)                                                                         \
    X(tier1_top, 10, 1, 128)                                                                                 \
    X(tier2_top, 30, 1, 128)                                                                                 \
    X(promotion_days, 90, 1, 365)                                                                            \
    X(frame_max_age_days, 7, 1, 365)                                                                         \
    X(claim_weight, 40, 0, 1000)                                                                             \
    X(plotter_weight, 30, 0, 1000)                                                                           \
    X(player_link_weight, 20, 0, 1000)                                                                       \
    X(honor_rank_weight, 5, 0, 1000)                                                                         \
    X(head_died_weight, 25, 0, 1000)                                                                         \
    X(player_link_days, 365, 1, 3650)                                                                        \
    X(head_died_days, 30, 1, 365)
struct ScheduleRules {
#define PAT_AI_DECLARE(name, def, lo, hi) std::int32_t name = 0;
    PAT_AI_SCHEDULE_FIELDS(PAT_AI_DECLARE)
#undef PAT_AI_DECLARE
    bool operator==(const ScheduleRules&) const = default;
};
#define PAT_AI_ATTENTION_FIELDS(X)                                                                           \
    X(base, 6, 1, 20)                                                                                        \
    X(court_divisor, 4, 1, 100)                                                                              \
    X(skill_divisor, 8, 1, 100)                                                                              \
    X(min_refill, 6, 1, 20)                                                                                  \
    X(max_refill, 20, 1, 20)                                                                                 \
    X(cap_mult, 2, 1, 2)                                                                                     \
    X(pass_min, 3, 1, 40)                                                                                    \
    X(pass_divisor, 2, 1, 40)
struct AttentionRules {
#define PAT_AI_DECLARE(name, def, lo, hi) std::int32_t name = 0;
    PAT_AI_ATTENTION_FIELDS(PAT_AI_DECLARE)
#undef PAT_AI_DECLARE
    bool operator==(const AttentionRules&) const = default;
};
#define PAT_AI_GOALS_FIELDS(X)                                                                               \
    X(office_base, 800, 0, 1000)                                                                             \
    X(office_per_holding, 250, 0, 1000)                                                                      \
    X(office_opportunity_mult, 10, 1, 1000)                                                                  \
    X(heir_absent, 1000, 0, 1000)                                                                            \
    X(heir_child, 600, 0, 1000)                                                                              \
    X(heir_adult, 200, 0, 1000)                                                                              \
    X(heir_adult_age, 16, 1, 100)                                                                            \
    X(marriage_per_adult, 250, 0, 1000)                                                                      \
    X(match_norm, 100, 1, 100000)                                                                            \
    X(reserve_months, 6, 1, 120)                                                                             \
    X(payback_norm_months, 12000, 1, 100000)                                                                 \
    X(favor_target, 70, 0, 100)                                                                              \
    X(favor_mult, 10, 1, 1000)                                                                               \
    X(favor_gift, 1000, 0, 1000)                                                                             \
    X(favor_service, 500, 0, 1000)                                                                           \
    X(suspicion_mult, 15, 0, 1000)                                                                           \
    X(disgraced_need, 300, 0, 1000)                                                                          \
    X(denounced_need, 500, 0, 1000)                                                                          \
    X(denounced_days, 90, 1, 365)                                                                            \
    X(protection_available, 1000, 0, 1000)                                                                   \
    X(protection_fallback, 300, 0, 1000)                                                                     \
    X(plotter_need, 900, 0, 1000)                                                                            \
    X(opportunist_need, 300, 0, 1000)                                                                        \
    X(loyalist_need, 0, 0, 1000)                                                                             \
    X(own_claim_need, 200, 0, 1000)                                                                          \
    X(plot_phase0, 200, 0, 1000)                                                                             \
    X(plot_phase1, 600, 0, 1000)                                                                             \
    X(plot_phase2, 1000, 0, 1000)                                                                            \
    X(claim_strength_min, 35, 0, 100)                                                                        \
    X(unrest_mult, 10, 0, 1000)                                                                              \
    X(rebels_need, 500, 0, 1000)                                                                             \
    X(army_opportunity, 1000, 0, 1000)                                                                       \
    X(tax_opportunity, 300, 0, 1000)                                                                         \
    X(attack_probability_min_pm, 650, 0, 1000)                                                               \
    X(min_utility, 50, 0, 10000)                                                                             \
    X(ap_price, 10, 1, 10000)                                                                                \
    X(money_price_kop, 1000, 1, 1000000)
struct GoalsRules {
#define PAT_AI_DECLARE(name, def, lo, hi) std::int32_t name = 0;
    PAT_AI_GOALS_FIELDS(PAT_AI_DECLARE)
#undef PAT_AI_DECLARE
    bool operator==(const GoalsRules&) const = default;
};
#define PAT_AI_HYSTERESIS_FIELDS(X)                                                                          \
    X(hold_pm, 1150, 1000, 3000)                                                                             \
    X(min_hold_days, 14, 0, 365)
struct HysteresisRules {
#define PAT_AI_DECLARE(name, def, lo, hi) std::int32_t name = 0;
    PAT_AI_HYSTERESIS_FIELDS(PAT_AI_DECLARE)
#undef PAT_AI_DECLARE
    bool operator==(const HysteresisRules&) const = default;
};
#define PAT_AI_COMMITMENT_FIELDS(X)                                                                          \
    X(floor_pm, 500, 0, 1000)                                                                                \
    X(max_days, 365, 1, 3650)
struct CommitmentRules {
#define PAT_AI_DECLARE(name, def, lo, hi) std::int32_t name = 0;
    PAT_AI_COMMITMENT_FIELDS(PAT_AI_DECLARE)
#undef PAT_AI_DECLARE
    bool operator==(const CommitmentRules&) const = default;
};
#define PAT_AI_COOLDOWNS_FIELDS(X)                                                                           \
    X(marriage_refused_days, 180, 1, 3650)                                                                   \
    X(plot_refused_days, 120, 1, 3650)                                                                       \
    X(race_days, 7, 1, 365)
struct CooldownsRules {
#define PAT_AI_DECLARE(name, def, lo, hi) std::int32_t name = 0;
    PAT_AI_COOLDOWNS_FIELDS(PAT_AI_DECLARE)
#undef PAT_AI_DECLARE
    bool operator==(const CooldownsRules&) const = default;
};
#define PAT_AI_ARCHETYPE_CHANGE_FIELDS(X)                                                                    \
    X(lead_pm, 200, 0, 3000)                                                                                 \
    X(same_faith_bonus, 100, 0, 1000)                                                                        \
    X(steward_unrest_min, 40, 0, 100)
struct ArchetypeChangeRules {
#define PAT_AI_DECLARE(name, def, lo, hi) std::int32_t name = 0;
    PAT_AI_ARCHETYPE_CHANGE_FIELDS(PAT_AI_DECLARE)
#undef PAT_AI_DECLARE
    bool operator==(const ArchetypeChangeRules&) const = default;
};
#define PAT_AI_COMMAND_COSTS_FIELDS(X)                                                                       \
    X(send_marriage_envoy, 3, 0, 40)                                                                         \
    X(betroth, 2, 0, 40)                                                                                     \
    X(set_heir, 2, 0, 40)                                                                                    \
    X(set_education, 1, 0, 40)                                                                               \
    X(petition_for_office, 3, 0, 40)                                                                         \
    X(gift_to_emperor, 2, 0, 40)                                                                             \
    X(hire_servant, 1, 0, 40)                                                                                \
    X(fire_servant, 1, 0, 40)                                                                                \
    X(assign_office, 1, 0, 40)                                                                               \
    X(start_plot, 5, 0, 40)                                                                                  \
    X(join_plot, 3, 0, 40)                                                                                   \
    X(contribute_to_plot, 2, 0, 40)                                                                          \
    X(leave_plot, 1, 0, 40)                                                                                  \
    X(denounce, 3, 0, 40)                                                                                    \
    X(press_claim, 3, 0, 40)                                                                                 \
    X(support_claim, 3, 0, 40)                                                                               \
    X(set_tax_policy, 2, 0, 40)                                                                              \
    X(set_recruit_policy, 2, 0, 40)                                                                          \
    X(start_construction, 3, 0, 40)                                                                          \
    X(take_loan, 2, 0, 40)                                                                                   \
    X(repay_loan, 1, 0, 40)                                                                                  \
    X(assign_province_administrator, 1, 0, 40)                                                               \
    X(order_move, 1, 0, 40)                                                                                  \
    X(order_siege, 1, 0, 40)                                                                                 \
    X(assign_to_front, 2, 0, 40)                                                                             \
    X(raise_army, 3, 0, 40)                                                                                  \
    X(set_faith_stance, 6, 0, 40)                                                                            \
    X(donate_to_church, 2, 0, 40)                                                                            \
    X(respond_marriage, 0, 0, 40)                                                                            \
    X(choose_event_option, 0, 0, 40)
struct CommandCostsRules {
#define PAT_AI_DECLARE(name, def, lo, hi) std::int32_t name = 0;
    PAT_AI_COMMAND_COSTS_FIELDS(PAT_AI_DECLARE)
#undef PAT_AI_DECLARE
    bool operator==(const CommandCostsRules&) const = default;
};
struct ArchetypeRules {
    std::array<std::int32_t, 7> weights{};
    std::int32_t risk_pm = 0;
    std::int32_t forgiveness_pm = 0;
    std::int32_t gift_share_pm = 0;
    std::int32_t denounce_bias_pm = 0;
    std::int32_t faith_bias_pm = 0;
    std::int32_t reform_bias_pm = 0;
    std::int32_t front_eagerness_pm = 0;
    std::int32_t tax_bias = 0;
    bool operator==(const ArchetypeRules&) const = default;
};
struct DifficultyRules {
    std::int32_t ap_mult_pm = 0;
    std::int32_t noise_pm = 0;
    std::int32_t aggression_pm = 0;
    std::int32_t player_bias_pm = 0;
    std::int32_t forgiveness_pm = 0;
    std::int32_t coalition_pm = 0;
    bool operator==(const DifficultyRules&) const = default;
};
struct EventGoalSignature {
    std::string effect;
    GoalKind goal = GoalKind::GainOffice;
    std::int32_t weight_pm = 0;
    bool operator==(const EventGoalSignature&) const = default;
};
struct AiRules {
    BudgetRules budget;
    ScheduleRules schedule;
    AttentionRules attention;
    GoalsRules goals;
    HysteresisRules hysteresis;
    CommitmentRules commitment;
    CooldownsRules cooldowns;
    ArchetypeChangeRules archetype_change;
    CommandCostsRules command_costs;
    std::array<ArchetypeRules, 8> archetypes{};
    std::array<DifficultyRules, 3> difficulty{};
    // Four ascending treasury thresholds in kopecks define five public wealth bands.
    std::array<std::int64_t, 4> wealth_bands{};
    std::vector<EventGoalSignature> event_goal_signatures;
    bool archetype_follows_head = false;
    [[nodiscard]] static AiRules defaults();
    [[nodiscard]] std::uint64_t hash() const noexcept;
    bool operator==(const AiRules&) const = default;
};
struct AiRulesError {
    std::string message;
    std::string field;
};
[[nodiscard]] core::Result<void, AiRulesError> validate(const AiRules& rules);
}  // namespace pat::sim::ai
