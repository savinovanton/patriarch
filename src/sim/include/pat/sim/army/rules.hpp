#pragma once

#include "pat/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Proposed M1 balance, docs/dev/army-model.md sections 9 and 12. This list is the authority for
// scalar defaults, value corridors, loading and hashing; representation schemas stay in pat::data.
#define PAT_ARMY_BALANCE_FIELDS(X)                                                                           \
    X(limits, world, 300, 1, 300)                                                                            \
    X(limits, dynasty, 5, 1, 300)                                                                            \
    X(limits, crown, 10, 1, 300)                                                                             \
    X(limits, rebels, 40, 1, 300)                                                                            \
    X(limits, min_size, 500, 1, 40000)                                                                       \
    X(limits, min_rebels, 200, 1, 40000)                                                                     \
    X(limits, max_size, 40000, 1, 40000)                                                                     \
    X(movement, slow_share_pm, 50, 1, 1000)                                                                  \
    X(movement, road_pm, 1250, 1, 4000)                                                                      \
    X(movement, march_drain_x100, 100, 0, 10000)                                                             \
    X(battle, loss_loser, 200, 0, 1000)                                                                      \
    X(battle, loss_winner, 120, 0, 1000)                                                                     \
    X(battle, max_loss, 950, 0, 1000)                                                                        \
    X(battle, morale_loser, 25, 0, 100)                                                                      \
    X(battle, morale_winner, 5, 0, 100)                                                                      \
    X(battle, p_min, 30, 0, 500)                                                                             \
    X(battle, p_max, 970, 500, 1000)                                                                         \
    X(battle, quality_base, 800, 1, 2000)                                                                    \
    X(battle, quality_factor, 4, 0, 100)                                                                     \
    X(battle, experience_base, 1000, 1, 2000)                                                                \
    X(battle, morale_base, 500, 1, 1000)                                                                     \
    X(battle, supply_base, 700, 1, 1000)                                                                     \
    X(battle, commander_gain, 300, 0, 2000)                                                                  \
    X(battle, commander_pivot, 10, 1, 100)                                                                   \
    X(battle, fort_bonus, 150, 0, 2000)                                                                      \
    X(battle, weather_pm, 1000, 1, 3000)                                                                     \
    X(battle, surprise_pm, 1000, 1, 3000)                                                                    \
    X(battle, no_retreat_extra_loss_pm, 300, 0, 1000)                                                        \
    X(battle, experience_winner, 2, 0, 100)                                                                  \
    X(battle, experience_loser, 1, 0, 100)                                                                   \
    X(battle, kill_loser_pm, 50, 0, 1000)                                                                    \
    X(battle, wound_loser_pm, 150, 0, 1000)                                                                  \
    X(battle, kill_winner_pm, 5, 0, 1000)                                                                    \
    X(battle, wound_winner_pm, 30, 0, 1000)                                                                  \
    X(battle, wound_health, -25, -100, 0)                                                                    \
    X(battle, honor_winner, 3, -100, 100)                                                                    \
    X(battle, honor_loser, -2, -100, 100)                                                                    \
    X(battle, favor_rebels, 1, -100, 100)                                                                    \
    X(supply, peasants_share_pm, 300, 0, 1000)                                                               \
    X(supply, townsfolk_share_pm, 400, 0, 1000)                                                              \
    X(supply, cossacks_share_pm, 400, 0, 1000)                                                               \
    X(supply, own_pm, 1000, 0, 2000)                                                                         \
    X(supply, friendly_pm, 600, 0, 2000)                                                                     \
    X(supply, foreign_pm, 400, 0, 2000)                                                                      \
    X(supply, hostile_pm, 300, 0, 2000)                                                                      \
    X(supply, rebels_home_pm, 700, 0, 2000)                                                                  \
    X(supply, regen_x100, 200, 0, 10000)                                                                     \
    X(supply, drain_x100, 600, 0, 10000)                                                                     \
    X(supply, hunger_x100, 300, 0, 10000)                                                                    \
    X(supply, attrition_below_x100, 5000, 0, 10000)                                                          \
    X(supply, attr_max_ppm, 8000, 0, 1000000)                                                                \
    X(supply, winter_pm, 1500, 1, 3000)                                                                      \
    X(siege, garrison_def, 500, 0, 3000)                                                                     \
    X(siege, wall_dmg, 250, 0, 3000)                                                                         \
    X(siege, breach_wall, 600, 0, 1000)                                                                      \
    X(siege, assault_min_progress, 300, 0, 1000)                                                             \
    X(siege, stores_days, 90, 1, 3650)                                                                       \
    X(siege, assault_loss_base, 2000, 1000, 4000)                                                            \
    X(siege, besieger_loss_pm, 6, 0, 1000)                                                                   \
    X(siege, garrison_loss_pm, 8, 0, 1000)                                                                   \
    X(siege, hunger_loss_pm, 12, 0, 1000)                                                                    \
    X(siege, starving_days, 30, 1, 3650)                                                                     \
    X(siege, winter_pm, 1500, 1, 3000)                                                                       \
    X(siege, sally_share_pm, 300, 0, 1000)                                                                   \
    X(siege, failed_progress, 200, 0, 1000)                                                                  \
    X(siege, failed_morale, 15, 0, 100)                                                                      \
    X(siege, garrison_after_retake, 100, 0, 40000)                                                           \
    X(siege, retake_control, 10, -100, 100)                                                                  \
    X(siege, retake_unrest, -10, -100, 100)                                                                  \
    X(siege, assault_unrest, 5, -100, 100)                                                                   \
    X(upkeep, quality_factor, 3, 0, 100)                                                                     \
    X(upkeep, paid_regen_x100, 500, 0, 10000)                                                                \
    X(upkeep, paid_supply_x100, 7000, 0, 10000)                                                              \
    X(upkeep, reliable_morale_x100, 4000, 0, 10000)                                                          \
    X(upkeep, desert_morale_x100, 3000, 0, 10000)                                                            \
    X(upkeep, desert_morale_factor, 2, 0, 100)                                                               \
    X(upkeep, unreliable_months, 3, 1, 255)                                                                  \
    X(front, aggr, 150, 0, 2000)                                                                             \
    X(front, noise_x100, 60, 0, 10000)                                                                       \
    X(front, effect, 500, 0, 3000)                                                                           \
    X(front, supply_x100, 8000, 0, 10000)                                                                    \
    X(front, call_min_size, 1000, 1, 40000)                                                                  \
    X(front, call_grace_days, 90, 1, 3650)                                                                   \
    X(front, high, 8000, 0, 10000)                                                                           \
    X(front, low, 2000, 0, 10000)                                                                            \
    X(front, hysteresis, 500, 0, 5000)                                                                       \
    X(front, enemy_pull_pm, 100, 0, 1000)                                                                    \
    X(front, peace_decay_pm, 100, 0, 1000)                                                                   \
    X(front, loss_base_ppm, 6000, 0, 1000000)                                                                \
    X(front, loss_pressure_ppm, 10000, 0, 1000000)                                                           \
    X(front, fatigue_pull_pm, 150, 0, 1000)                                                                  \
    X(front, fatigue_war_x100, 1500, 0, 10000)                                                               \
    X(front, fatigue_pressure_pm, 200, 0, 1000)                                                              \
    X(front, fatigue_losses_pm, 100, 0, 1000)                                                                \
    X(front, service_pm, 200, 0, 1000)                                                                       \
    X(front, honor_cap, 10, 0, 100)                                                                          \
    X(front, call_suspicion_x100, 300, 0, 10000)                                                             \
    X(front, return_days, 20, 0, 365)                                                                        \
    X(front, war_zero_months, 3, 1, 120)                                                                     \
    X(front, truce_months, 6, 1, 120)                                                                        \
    X(rebels, share_min, 20, 0, 1000)                                                                        \
    X(rebels, share_max, 80, 0, 1000)                                                                        \
    X(rebels, infantry_pm, 100, 0, 1000)                                                                     \
    X(rebels, cavalry_max_pm, 300, 0, 900)                                                                   \
    X(rebels, cavalry_noise_divisor, 10, 1, 1000)                                                            \
    X(rebels, quality_base, 10, 0, 100)                                                                      \
    X(rebels, quality_range, 20, 1, 100)                                                                     \
    X(rebels, dissolve_size, 200, 1, 40000)                                                                  \
    X(rebels, max_months, 24, 1, 120)                                                                        \
    X(rebels, decay_pm, 15, 0, 1000)                                                                         \
    X(rebels, cooldown_days, 180, 1, 3650)                                                                   \
    X(rebels, spread_unrest, 60, 0, 100)                                                                     \
    X(rebels, range_edges, 2, 1, 400)                                                                        \
    X(rebels, suppress_unrest, -20, -100, 0)                                                                 \
    X(rebels, suppress_control, 10, 0, 100)                                                                  \
    X(rebels, unrest_start, 75, 0, 99)                                                                       \
    X(rebels, move_min_size, 400, 1, 40000)                                                                  \
    X(rebels, control_monthly, -3, -100, 100)                                                                \
    X(rebels, unrest_monthly, 2, -100, 100)                                                                  \
    X(rebels, cruel_loss_pm, 500, 0, 1000)                                                                   \
    X(rebels, cruel_mood, -5, -100, 0)                                                                       \
    X(rebels, capture_garrison_pm, 250, 0, 1000)                                                             \
    X(rebels, capture_stores_days, 60, 1, 3650)                                                              \
    X(levy, share, 100, 0, 1000)                                                                             \
    X(levy, retainers, 30, 1, 1000)                                                                          \
    X(levy, cavalry_pm, 300, 0, 1000)                                                                        \
    X(levy, days, 90, 1, 365)                                                                                \
    X(levy, cooldown_days, 180, 1, 3650)                                                                     \
    X(levy, mood_nobles, -8, -100, 0)                                                                        \
    X(levy, quality, 20, 0, 100)                                                                             \
    X(levy, min_size, 300, 1, 40000)                                                                         \
    X(levy, morale_x100, 7000, 0, 10000)                                                                     \
    X(muster, quality_new, 30, 0, 100)                                                                       \
    X(muster, quality_trained, 55, 0, 100)                                                                   \
    X(muster, train_share_pm, 100, 1, 1000)                                                                  \
    X(muster, commander_age, 16, 1, 100)                                                                     \
    X(muster, commander_martial, 4, 0, 20)                                                                   \
    X(guard, target_size, 2000, 1, 40000)                                                                    \
    X(guard, morale_slope, 300, 0, 2000)                                                                     \
    X(guard, pay_step_x100, 400, 0, 10000)                                                                   \
    X(guard, shift_unreliable, -15, -100, 0)                                                                 \
    X(guard, shift_destroyed, -20, -100, 0)                                                                  \
    X(guard, morale_after_coup, 60, 0, 100)                                                                  \
    X(guard, morale_pivot_x100, 7000, 0, 10000)                                                              \
    X(guard, absent_x100, -1000, -10000, 0)                                                                  \
    X(ai, attack_min_pm, 650, 0, 1000)                                                                       \
    X(ai, assault_min_pm, 900, 0, 1000)                                                                      \
    X(ai, front_suspicion, 30, 0, 100)                                                                       \
    X(ai, front_favor, 60, 0, 100)

namespace pat::sim::army {
inline constexpr std::size_t kBranchCount = 4;
inline constexpr std::size_t kTerrainCount = 6;
enum class Branch : std::uint8_t { Infantry = 0, Cavalry = 1, Artillery = 2, Irregular = 3 };
enum class Terrain : std::uint8_t { Plain = 1, Steppe = 2, Forest = 3, Swamp = 4, Hills = 5, Mountain = 6 };
enum class CounterClass : std::uint8_t { Open = 0, Rough = 1 };
inline constexpr std::array<std::string_view, 4> kBranchNames{
    "infantry", "cavalry", "artillery", "irregular"};
inline constexpr std::array<std::string_view, 6> kTerrainNames{
    "plain", "steppe", "forest", "swamp", "hills", "mountain"};

struct BranchRules {
    std::int32_t cv = 0, speed_kmd = 0, upkeep_kop = 0, equip_kop = 0;
    std::int32_t vuln = 0, need = 0, siege_value = 0, train_days = 0;
    bool operator==(const BranchRules&) const = default;
};
struct TerrainRules {
    std::int32_t move_pm = 0, defence_pm = 0, supply_pm = 0;
    CounterClass counter_class = CounterClass::Open;
    bool operator==(const TerrainRules&) const = default;
};
struct SeasonRules {
    std::int32_t move_pm = 0, supply_pm = 0;
    bool operator==(const SeasonRules&) const = default;
};
struct ArrearsRule {
    std::int32_t morale = 0, desert = 0;
    bool operator==(const ArrearsRule&) const = default;
};
struct ArmyRules {
#define PAT_ARMY_DECLARE(group, name, def, lo, hi) std::int32_t group##_##name = def;
    PAT_ARMY_BALANCE_FIELDS(PAT_ARMY_DECLARE)
#undef PAT_ARMY_DECLARE
    std::array<BranchRules, kBranchCount> branches{};
    std::array<TerrainRules, kTerrainCount> terrains{};
    std::array<SeasonRules, 12> seasons{};
    std::array<std::array<std::array<std::int32_t, 4>, 4>, 2> counter{};
    std::array<std::int32_t, 11> sigmoid{};
    std::array<std::int32_t, 6> siege_defence{}, fortress_upkeep{};
    std::array<std::int32_t, 4> front_enemy_base{};
    std::array<ArrearsRule, 4> arrears{};
    [[nodiscard]] static ArmyRules defaults();
    /// Canonical hash of typed rules only; not installed in World or M1 compatibility until T20.
    [[nodiscard]] std::uint64_t hash() const noexcept;
    bool operator==(const ArmyRules&) const = default;
};
struct ArmyRulesError {
    std::string message;
    std::string field;
};
[[nodiscard]] core::Result<void, ArmyRulesError> validate(const ArmyRules& rules);
}  // namespace pat::sim::army
