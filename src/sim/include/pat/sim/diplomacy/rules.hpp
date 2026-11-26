#pragma once
#include "pat/core/result.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
namespace pat::sim::diplomacy {
inline constexpr std::array<std::string_view, 4> kTreatyNames{
    "alliance", "pledge", "marriage_pact", "claim_support"};
inline constexpr std::array<std::string_view, 4> kReasonNames{
    "offer_refused", "denounced", "false_denunciation", "honoured"};
#define PAT_DIPLOMACY_BALANCE_FIELDS(X)                                                                      \
    X(opinion, kin_weight_pm, 700, 0, 1000)                                                                  \
    X(opinion, head_weight_pm, 300, 0, 1000)                                                                 \
    X(opinion, feud_enter, -60, -100, 100)                                                                   \
    X(opinion, feud_exit, -50, -100, 100)                                                                    \
    X(trust, default, 50, 0, 100)                                                                            \
    X(trust, treaty_ceiling, 80, 0, 100)                                                                     \
    X(trust, public_breach, -5, -100, 0)                                                                     \
    X(trust, denounced, -10, -100, 0)                                                                        \
    X(trust, honoured, 5, 0, 100)                                                                            \
    X(limits, relations_per_house, 40, 1, 40)                                                                \
    X(limits, mods_per_relation, 6, 1, 6)                                                                    \
    X(limits, treaties_per_house, 8, 1, 8)                                                                   \
    X(limits, offers_per_pair, 3, 1, 3)                                                                      \
    X(limits, player_inbox_cap, 2, 1, 2)                                                                     \
    X(limits, favors_owed_max, 3, 1, 3)                                                                      \
    X(offers, answer_days, 30, 1, 30)                                                                        \
    X(offers, refusal_cooldown_days, 180, 1, 3650)                                                           \
    X(offers, break_cooldown_days, 730, 1, 3650)                                                             \
    X(offers, ai_offer_cooldown_days, 90, 1, 3650)                                                           \
    X(offers, counter_margin, 15, 0, 100)                                                                    \
    X(accept, opinion_pm, 500, 0, 1000)                                                                      \
    X(accept, trust_pm, 400, 0, 1000)                                                                        \
    X(accept, emperor_pm, 300, 0, 1000)                                                                      \
    X(accept, grudge_pm, 250, 0, 1000)                                                                       \
    X(accept, threshold, 20, -100, 100)                                                                      \
    X(accept, favor_call, 30, 0, 100)                                                                        \
    X(accept, value_cap, 40, 1, 100)                                                                         \
    X(accept, forecast_margin, 10, 0, 100)                                                                   \
    X(gift, min_rub, 10, 1, 1000000)                                                                         \
    X(gift, max_rub, 2000, 1, 1000000)                                                                       \
    X(gift, opinion_per_100rub, 8, 1, 100)                                                                   \
    X(gift, cap, 20, 1, 100)                                                                                 \
    X(gift, months, 12, 6, 60)                                                                               \
    X(gift, cooldown_days, 30, 1, 3650)                                                                      \
    X(favor, min_rub, 50, 1, 1000000)                                                                        \
    X(favor, max_rub, 1000, 1, 1000000)                                                                      \
    X(favor, granted_opinion, 5, 0, 100)                                                                     \
    X(favor, ingratitude, -10, -100, 0)                                                                      \
    X(favor, expiry_months, 120, 1, 120)                                                                     \
    X(favor, granted_months, 12, 6, 60)                                                                      \
    X(favor, ingratitude_months, 24, 24, 60)                                                                 \
    X(pledge, min_rub, 100, 1, 1000000)                                                                      \
    X(pledge, max_rub, 5000, 1, 1000000)                                                                     \
    X(archetype_threshold, schemer, 5, -100, 100)                                                            \
    X(archetype_threshold, loyalist_with_loyalist, -5, -100, 100)                                            \
    X(ai, min_treaty_age_days, 365, 1, 3650)                                                                 \
    X(ai, propose_wu, 40, 1, 10000)                                                                          \
    X(ai, gift_wu, 20, 1, 10000)                                                                             \
    X(ai, favor_wu, 20, 1, 10000)
#define PAT_DIPLOMACY_TREATY_FIELDS(X)                                                                       \
    X(min_months, 0, 0, 120)                                                                                 \
    X(months, 0, 0, 120)                                                                                     \
    X(max_months, 0, 0, 120)                                                                                 \
    X(opinion_bonus, 0, -100, 100)                                                                           \
    X(base_value, 0, -100, 100)                                                                              \
    X(risk, 0, 0, 100)                                                                                       \
    X(political_gain, 0, 0, 30)                                                                              \
    X(breach_trust, 0, -100, 0)                                                                              \
    X(breach_grudge, 0, -100, 0)                                                                             \
    X(breach_months, 24, 24, 60)

struct TreatyRule {
#define PAT_DIP_DECLARE(name, def, lo, hi) std::int32_t name = def;
    PAT_DIPLOMACY_TREATY_FIELDS(PAT_DIP_DECLARE)
#undef PAT_DIP_DECLARE
    bool enabled = true;
    bool system_only = false;
    bool breakable = true;
    bool ai_may_offer_to_player = false;
    bool operator==(const TreatyRule&) const = default;
};
struct RelationRule {
    std::int32_t value = 0, months = 12;
    bool operator==(const RelationRule&) const = default;
};
inline constexpr std::array<std::string_view, 6> kTraitHintNames{
    "cautious", "proud", "ambitious", "generous", "diplomat", "greedy"};
struct TraitHintRule {
    std::int32_t dipl_threshold = 0, dipl_greed = 0, dipl_arrogance = 0;
    bool operator==(const TraitHintRule&) const = default;
};
struct DiplomacyRules {
#define PAT_DIP_DECLARE(group, name, def, lo, hi) std::int32_t group##_##name = def;
    PAT_DIPLOMACY_BALANCE_FIELDS(PAT_DIP_DECLARE)
#undef PAT_DIP_DECLARE
    bool ai_may_offer_to_player = true;
    std::array<TreatyRule, kTreatyNames.size()> treaties{};
    std::array<RelationRule, kReasonNames.size()> reasons{};
    std::array<TraitHintRule, kTraitHintNames.size()> trait_hints{};
    [[nodiscard]] static DiplomacyRules defaults();
    /// Canonical typed hash; runtime installation and compatibility key 26 belong to E15-T17.
    [[nodiscard]] std::uint64_t hash() const noexcept;
    bool operator==(const DiplomacyRules&) const = default;
};
struct DiplomacyRulesError {
    std::string message, field;
};
[[nodiscard]] core::Result<void, DiplomacyRulesError> validate(const DiplomacyRules& rules);
}  // namespace pat::sim::diplomacy
