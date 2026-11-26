#include "pat/sim/ai/rules.hpp"

#include "pat/core/hash_stream.hpp"
#include "pat/sim/rules_check.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <utility>
namespace pat::sim::ai {
AiRules AiRules::defaults() {
    AiRules out;
#define PAT_AI_DEFAULT(name, def, lo, hi) out.budget.name = def;
    PAT_AI_BUDGET_FIELDS(PAT_AI_DEFAULT)
#undef PAT_AI_DEFAULT
#define PAT_AI_DEFAULT(name, def, lo, hi) out.schedule.name = def;
    PAT_AI_SCHEDULE_FIELDS(PAT_AI_DEFAULT)
#undef PAT_AI_DEFAULT
#define PAT_AI_DEFAULT(name, def, lo, hi) out.attention.name = def;
    PAT_AI_ATTENTION_FIELDS(PAT_AI_DEFAULT)
#undef PAT_AI_DEFAULT
#define PAT_AI_DEFAULT(name, def, lo, hi) out.goals.name = def;
    PAT_AI_GOALS_FIELDS(PAT_AI_DEFAULT)
#undef PAT_AI_DEFAULT
#define PAT_AI_DEFAULT(name, def, lo, hi) out.hysteresis.name = def;
    PAT_AI_HYSTERESIS_FIELDS(PAT_AI_DEFAULT)
#undef PAT_AI_DEFAULT
#define PAT_AI_DEFAULT(name, def, lo, hi) out.commitment.name = def;
    PAT_AI_COMMITMENT_FIELDS(PAT_AI_DEFAULT)
#undef PAT_AI_DEFAULT
#define PAT_AI_DEFAULT(name, def, lo, hi) out.cooldowns.name = def;
    PAT_AI_COOLDOWNS_FIELDS(PAT_AI_DEFAULT)
#undef PAT_AI_DEFAULT
#define PAT_AI_DEFAULT(name, def, lo, hi) out.archetype_change.name = def;
    PAT_AI_ARCHETYPE_CHANGE_FIELDS(PAT_AI_DEFAULT)
#undef PAT_AI_DEFAULT
#define PAT_AI_DEFAULT(name, def, lo, hi) out.command_costs.name = def;
    PAT_AI_COMMAND_COSTS_FIELDS(PAT_AI_DEFAULT)
#undef PAT_AI_DEFAULT
    out.archetypes[0] = {{1400, 1000, 900, 1100, 800, 1200, 800}, 1200, 1000, 50, 1000, 1000, 1000, 1000, 0};
    out.archetypes[1] = {{1000, 1000, 800, 1300, 900, 200, 1100}, 1000, 1000, 70, 1500, 1000, 1000, 1000, 0};
    out.archetypes[2] = {{700, 1000, 1400, 800, 1200, 400, 1100}, 700, 1000, 50, 1000, 1000, 1000, 1000, -1};
    out.archetypes[3] = {{1000, 900, 800, 900, 1100, 1500, 600}, 1300, 1000, 50, 1000, 1000, 1000, 1000, 0};
    out.archetypes[4] = {{1100, 900, 1500, 900, 1000, 800, 900}, 1000, 1000, 30, 1000, 1000, 1000, 1000, 1};
    out.archetypes[5] = {{700, 1300, 700, 1000, 1100, 400, 900}, 1000, 1000, 50, 1000, 1500, 1000, 1000, 0};
    out.archetypes[6] = {{1100, 1000, 700, 1000, 700, 900, 1500}, 1100, 1000, 50, 1000, 1000, 1000, 1500, 0};
    out.archetypes[7] = {{1300, 900, 1000, 1300, 900, 500, 1000}, 1000, 1000, 50, 1000, 1000, 1300, 1000, 0};
    out.difficulty[0] = {800, 150, 700, 800, 700, 700};
    out.difficulty[1] = {1000, 100, 1000, 1000, 1000, 1000};
    out.difficulty[2] = {1200, 50, 1300, 1200, 1400, 1300};
    // Provisional public bands; calibration and domain event signatures belong to T16/T18.
    out.wealth_bands = {0, 10000, 100000, 1000000};
    out.archetype_follows_head = true;
    return out;
}
std::uint64_t AiRules::hash() const noexcept {
    core::HashStream h;
#define PAT_AI_HASH(name, def, lo, hi) h.i32(budget.name);
    PAT_AI_BUDGET_FIELDS(PAT_AI_HASH)
#undef PAT_AI_HASH
#define PAT_AI_HASH(name, def, lo, hi) h.i32(schedule.name);
    PAT_AI_SCHEDULE_FIELDS(PAT_AI_HASH)
#undef PAT_AI_HASH
#define PAT_AI_HASH(name, def, lo, hi) h.i32(attention.name);
    PAT_AI_ATTENTION_FIELDS(PAT_AI_HASH)
#undef PAT_AI_HASH
#define PAT_AI_HASH(name, def, lo, hi) h.i32(goals.name);
    PAT_AI_GOALS_FIELDS(PAT_AI_HASH)
#undef PAT_AI_HASH
#define PAT_AI_HASH(name, def, lo, hi) h.i32(hysteresis.name);
    PAT_AI_HYSTERESIS_FIELDS(PAT_AI_HASH)
#undef PAT_AI_HASH
#define PAT_AI_HASH(name, def, lo, hi) h.i32(commitment.name);
    PAT_AI_COMMITMENT_FIELDS(PAT_AI_HASH)
#undef PAT_AI_HASH
#define PAT_AI_HASH(name, def, lo, hi) h.i32(cooldowns.name);
    PAT_AI_COOLDOWNS_FIELDS(PAT_AI_HASH)
#undef PAT_AI_HASH
#define PAT_AI_HASH(name, def, lo, hi) h.i32(archetype_change.name);
    PAT_AI_ARCHETYPE_CHANGE_FIELDS(PAT_AI_HASH)
#undef PAT_AI_HASH
#define PAT_AI_HASH(name, def, lo, hi) h.i32(command_costs.name);
    PAT_AI_COMMAND_COSTS_FIELDS(PAT_AI_HASH)
#undef PAT_AI_HASH
    for (const auto& item : archetypes) {
        for (auto weight : item.weights)
            h.i32(weight);
        h.i32(item.risk_pm);
        h.i32(item.forgiveness_pm);
        h.i32(item.gift_share_pm);
        h.i32(item.denounce_bias_pm);
        h.i32(item.faith_bias_pm);
        h.i32(item.reform_bias_pm);
        h.i32(item.front_eagerness_pm);
        h.i32(item.tax_bias);
    }
    for (const auto& item : difficulty) {
        h.i32(item.ap_mult_pm);
        h.i32(item.noise_pm);
        h.i32(item.aggression_pm);
        h.i32(item.player_bias_pm);
        h.i32(item.forgiveness_pm);
        h.i32(item.coalition_pm);
    }
    for (auto band : wealth_bands)
        h.i64(band);
    h.boolean(archetype_follows_head);
    h.u64(event_goal_signatures.size());
    for (const auto& signature : event_goal_signatures) {
        h.u64(signature.effect.size());
        for (const auto c : signature.effect)
            h.u8(static_cast<std::uint8_t>(c));
        h.u8(static_cast<std::uint8_t>(signature.goal));
        h.i32(signature.weight_pm);
    }
    return h.value();
}
core::Result<void, AiRulesError> validate(const AiRules& rules) {
    using namespace rules_check;
#define PAT_AI_CHECK(name, def, lo, hi)                                                                      \
    PAT_RULES_CHECK(in_range<AiRulesError>("budget." #name, rules.budget.name, lo, hi));
    PAT_AI_BUDGET_FIELDS(PAT_AI_CHECK)
#undef PAT_AI_CHECK
#define PAT_AI_CHECK(name, def, lo, hi)                                                                      \
    PAT_RULES_CHECK(in_range<AiRulesError>("schedule." #name, rules.schedule.name, lo, hi));
    PAT_AI_SCHEDULE_FIELDS(PAT_AI_CHECK)
#undef PAT_AI_CHECK
#define PAT_AI_CHECK(name, def, lo, hi)                                                                      \
    PAT_RULES_CHECK(in_range<AiRulesError>("attention." #name, rules.attention.name, lo, hi));
    PAT_AI_ATTENTION_FIELDS(PAT_AI_CHECK)
#undef PAT_AI_CHECK
#define PAT_AI_CHECK(name, def, lo, hi)                                                                      \
    PAT_RULES_CHECK(in_range<AiRulesError>("goals." #name, rules.goals.name, lo, hi));
    PAT_AI_GOALS_FIELDS(PAT_AI_CHECK)
#undef PAT_AI_CHECK
#define PAT_AI_CHECK(name, def, lo, hi)                                                                      \
    PAT_RULES_CHECK(in_range<AiRulesError>("hysteresis." #name, rules.hysteresis.name, lo, hi));
    PAT_AI_HYSTERESIS_FIELDS(PAT_AI_CHECK)
#undef PAT_AI_CHECK
#define PAT_AI_CHECK(name, def, lo, hi)                                                                      \
    PAT_RULES_CHECK(in_range<AiRulesError>("commitment." #name, rules.commitment.name, lo, hi));
    PAT_AI_COMMITMENT_FIELDS(PAT_AI_CHECK)
#undef PAT_AI_CHECK
#define PAT_AI_CHECK(name, def, lo, hi)                                                                      \
    PAT_RULES_CHECK(in_range<AiRulesError>("cooldowns." #name, rules.cooldowns.name, lo, hi));
    PAT_AI_COOLDOWNS_FIELDS(PAT_AI_CHECK)
#undef PAT_AI_CHECK
#define PAT_AI_CHECK(name, def, lo, hi)                                                                      \
    PAT_RULES_CHECK(in_range<AiRulesError>("archetype_change." #name, rules.archetype_change.name, lo, hi));
    PAT_AI_ARCHETYPE_CHANGE_FIELDS(PAT_AI_CHECK)
#undef PAT_AI_CHECK
#define PAT_AI_CHECK(name, def, lo, hi)                                                                      \
    PAT_RULES_CHECK(in_range<AiRulesError>("command_costs." #name, rules.command_costs.name, lo, hi));
    PAT_AI_COMMAND_COSTS_FIELDS(PAT_AI_CHECK)
#undef PAT_AI_CHECK
    const auto ordered = [](bool condition, std::string field) {
        return condition ? Check<AiRulesError>::ok()
                         : fail<AiRulesError>("inconsistent AI rule bounds", std::move(field));
    };
    PAT_RULES_CHECK(ordered(rules.schedule.tier1_days <= rules.schedule.tier2_days &&
                                rules.schedule.tier2_days <= rules.schedule.tier3_days,
                            "schedule.tier2_days"));
    PAT_RULES_CHECK(ordered(rules.schedule.tier1_threshold >= rules.schedule.tier2_threshold,
                            "schedule.tier2_threshold"));
    PAT_RULES_CHECK(ordered(rules.schedule.tier1_top <= rules.schedule.tier2_top, "schedule.tier2_top"));
    PAT_RULES_CHECK(
        ordered(rules.attention.min_refill <= rules.attention.max_refill, "attention.max_refill"));
    for (std::size_t i = 0; i < rules.archetypes.size(); ++i) {
        const auto& item = rules.archetypes[i];
        const auto prefix = std::format("archetypes.{}.", kArchetypeKeys[i]);
        for (std::size_t goal = 0; goal < item.weights.size(); ++goal)
            PAT_RULES_CHECK(in_range<AiRulesError>(
                prefix + "weights." + std::string(kGoalKeys[goal]), item.weights[goal], 0, 3000));
        PAT_RULES_CHECK(in_range<AiRulesError>(prefix + "risk_pm", item.risk_pm, 0, 3000));
        PAT_RULES_CHECK(in_range<AiRulesError>(prefix + "forgiveness_pm", item.forgiveness_pm, 0, 3000));
        PAT_RULES_CHECK(in_range<AiRulesError>(prefix + "gift_share_pm", item.gift_share_pm, 0, 1000));
        PAT_RULES_CHECK(in_range<AiRulesError>(prefix + "denounce_bias_pm", item.denounce_bias_pm, 0, 3000));
        PAT_RULES_CHECK(in_range<AiRulesError>(prefix + "faith_bias_pm", item.faith_bias_pm, 0, 3000));
        PAT_RULES_CHECK(in_range<AiRulesError>(prefix + "reform_bias_pm", item.reform_bias_pm, 0, 3000));
        PAT_RULES_CHECK(
            in_range<AiRulesError>(prefix + "front_eagerness_pm", item.front_eagerness_pm, 0, 3000));
        PAT_RULES_CHECK(in_range<AiRulesError>(prefix + "tax_bias", item.tax_bias, -1, 1));
    }
    for (std::size_t i = 0; i < rules.difficulty.size(); ++i) {
        const auto& item = rules.difficulty[i];
        const auto prefix = std::format("difficulty.{}.", kDifficultyKeys[i]);
        PAT_RULES_CHECK(in_range<AiRulesError>(prefix + "ap_mult_pm", item.ap_mult_pm, 0, 3000));
        PAT_RULES_CHECK(in_range<AiRulesError>(prefix + "noise_pm", item.noise_pm, 0, 1000));
        PAT_RULES_CHECK(in_range<AiRulesError>(prefix + "aggression_pm", item.aggression_pm, 0, 3000));
        PAT_RULES_CHECK(in_range<AiRulesError>(prefix + "player_bias_pm", item.player_bias_pm, 0, 3000));
        PAT_RULES_CHECK(in_range<AiRulesError>(prefix + "forgiveness_pm", item.forgiveness_pm, 0, 3000));
        PAT_RULES_CHECK(in_range<AiRulesError>(prefix + "coalition_pm", item.coalition_pm, 0, 3000));
    }
    for (std::size_t i = 0; i < rules.wealth_bands.size(); ++i) {
        PAT_RULES_CHECK(in_range<AiRulesError>(
            std::format("wealth_bands[{}]", i), rules.wealth_bands[i], 0, 1000000000000LL));
        if (i > 0)
            PAT_RULES_CHECK(ordered(rules.wealth_bands[i - 1] < rules.wealth_bands[i],
                                    std::format("wealth_bands[{}]", i)));
    }
    std::set<std::pair<std::string, GoalKind>> seen;
    for (const auto& signature : rules.event_goal_signatures) {
        PAT_RULES_CHECK(
            ordered(!signature.effect.empty() && seen.emplace(signature.effect, signature.goal).second,
                    "event_goal_signatures"));
        PAT_RULES_CHECK(
            in_range<AiRulesError>("event_goal_signatures", static_cast<std::uint8_t>(signature.goal), 1, 7));
        PAT_RULES_CHECK(in_range<AiRulesError>("event_goal_signatures", signature.weight_pm, -300, 300));
    }
    return Check<AiRulesError>::ok();
}
}  // namespace pat::sim::ai
