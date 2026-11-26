#include "pat/data/ai_schemas.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
namespace pat::data {
namespace {
Type number() {
    return Type::integer(std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max());
}
}  // namespace
void add_ai_schemas(SchemaSet& set) {
    ObjectSchema rules("ai_rules");
    rules.intern_last().describe_kind(
        "Правила ИИ M1: семь целей, восемь архетипов и три уровня; включаются отдельно в E17-T19.");
    ObjectSchema budget("budget");
    budget.required("quota_wu", number());
    budget.required("carry_pm", number());
    budget.required("pass_wu", number());
    budget.required("goal_wu", number());
    budget.required("filter_rows", number());
    budget.required("filter_wu", number());
    budget.required("object_wu", number());
    budget.required("marriage_wu", number());
    budget.required("office_wu", number());
    budget.required("plot_wu", number());
    budget.required("construction_wu", number());
    budget.required("tax_wu", number());
    budget.required("loan_wu", number());
    budget.required("battle_wu", number());
    budget.required("emit_wu", number());
    budget.required("log_wu", number());
    rules.required("budget", Type::block(std::move(budget)));
    ObjectSchema schedule("schedule");
    schedule.required("tier1_days", number());
    schedule.required("tier2_days", number());
    schedule.required("tier3_days", number());
    schedule.required("tier1_threshold", number());
    schedule.required("tier2_threshold", number());
    schedule.required("tier1_top", number());
    schedule.required("tier2_top", number());
    schedule.required("promotion_days", number());
    schedule.required("frame_max_age_days", number());
    schedule.required("claim_weight", number());
    schedule.required("plotter_weight", number());
    schedule.required("player_link_weight", number());
    schedule.required("honor_rank_weight", number());
    schedule.required("head_died_weight", number());
    schedule.required("player_link_days", number());
    schedule.required("head_died_days", number());
    rules.required("schedule", Type::block(std::move(schedule)));
    ObjectSchema attention("attention");
    attention.required("base", number());
    attention.required("court_divisor", number());
    attention.required("skill_divisor", number());
    attention.required("min_refill", number());
    attention.required("max_refill", number());
    attention.required("cap_mult", number());
    attention.required("pass_min", number());
    attention.required("pass_divisor", number());
    rules.required("attention", Type::block(std::move(attention)));
    ObjectSchema goals("goals");
    goals.required("office_base", number());
    goals.required("office_per_holding", number());
    goals.required("office_opportunity_mult", number());
    goals.required("heir_absent", number());
    goals.required("heir_child", number());
    goals.required("heir_adult", number());
    goals.required("heir_adult_age", number());
    goals.required("marriage_per_adult", number());
    goals.required("match_norm", number());
    goals.required("reserve_months", number());
    goals.required("payback_norm_months", number());
    goals.required("favor_target", number());
    goals.required("favor_mult", number());
    goals.required("favor_gift", number());
    goals.required("favor_service", number());
    goals.required("suspicion_mult", number());
    goals.required("disgraced_need", number());
    goals.required("denounced_need", number());
    goals.required("denounced_days", number());
    goals.required("protection_available", number());
    goals.required("protection_fallback", number());
    goals.required("plotter_need", number());
    goals.required("opportunist_need", number());
    goals.required("loyalist_need", number());
    goals.required("own_claim_need", number());
    goals.required("plot_phase0", number());
    goals.required("plot_phase1", number());
    goals.required("plot_phase2", number());
    goals.required("claim_strength_min", number());
    goals.required("unrest_mult", number());
    goals.required("rebels_need", number());
    goals.required("army_opportunity", number());
    goals.required("tax_opportunity", number());
    goals.required("attack_probability_min_pm", number());
    goals.required("min_utility", number());
    goals.required("ap_price", number());
    goals.required("money_price_kop", number());
    rules.required("goals", Type::block(std::move(goals)));
    ObjectSchema hysteresis("hysteresis");
    hysteresis.required("hold_pm", number());
    hysteresis.required("min_hold_days", number());
    rules.required("hysteresis", Type::block(std::move(hysteresis)));
    ObjectSchema commitment("commitment");
    commitment.required("floor_pm", number());
    commitment.required("max_days", number());
    rules.required("commitment", Type::block(std::move(commitment)));
    ObjectSchema cooldowns("cooldowns");
    cooldowns.required("marriage_refused_days", number());
    cooldowns.required("plot_refused_days", number());
    cooldowns.required("race_days", number());
    rules.required("cooldowns", Type::block(std::move(cooldowns)));
    ObjectSchema archetype_change("archetype_change");
    archetype_change.required("lead_pm", number());
    archetype_change.required("same_faith_bonus", number());
    archetype_change.required("steward_unrest_min", number());
    rules.required("archetype_change", Type::block(std::move(archetype_change)));
    ObjectSchema command_costs("command_costs");
    command_costs.required("send_marriage_envoy", number());
    command_costs.required("betroth", number());
    command_costs.required("set_heir", number());
    command_costs.required("set_education", number());
    command_costs.required("petition_for_office", number());
    command_costs.required("gift_to_emperor", number());
    command_costs.required("hire_servant", number());
    command_costs.required("fire_servant", number());
    command_costs.required("assign_office", number());
    command_costs.required("start_plot", number());
    command_costs.required("join_plot", number());
    command_costs.required("contribute_to_plot", number());
    command_costs.required("leave_plot", number());
    command_costs.required("denounce", number());
    command_costs.required("press_claim", number());
    command_costs.required("support_claim", number());
    command_costs.required("set_tax_policy", number());
    command_costs.required("set_recruit_policy", number());
    command_costs.required("start_construction", number());
    command_costs.required("take_loan", number());
    command_costs.required("repay_loan", number());
    command_costs.required("assign_province_administrator", number());
    command_costs.required("order_move", number());
    command_costs.required("order_siege", number());
    command_costs.required("assign_to_front", number());
    command_costs.required("raise_army", number());
    command_costs.required("set_faith_stance", number());
    command_costs.required("donate_to_church", number());
    command_costs.required("respond_marriage", number());
    command_costs.required("choose_event_option", number());
    rules.required("command_costs", Type::block(std::move(command_costs)));
    ObjectSchema weights("weights");
    weights.required("gain_office", number());
    weights.required("secure_line", number());
    weights.required("grow_wealth", number());
    weights.required("gain_favor", number());
    weights.required("protect_self", number());
    weights.required("prepare_plot", number());
    weights.required("keep_order", number());
    ObjectSchema archetype("archetype");
    archetype.required("weights", Type::block(std::move(weights)));
    archetype.required("risk_pm", number());
    archetype.required("forgiveness_pm", number());
    archetype.required("gift_share_pm", number());
    archetype.required("denounce_bias_pm", number());
    archetype.required("faith_bias_pm", number());
    archetype.required("reform_bias_pm", number());
    archetype.required("front_eagerness_pm", number());
    archetype.required("tax_bias", number());
    ObjectSchema archetypes("archetypes");
    archetypes.required("ambitious", Type::block(archetype));
    archetypes.required("loyalist", Type::block(archetype));
    archetypes.required("steward", Type::block(archetype));
    archetypes.required("schemer", Type::block(archetype));
    archetypes.required("acquisitive", Type::block(archetype));
    archetypes.required("pious", Type::block(archetype));
    archetypes.required("warrior", Type::block(archetype));
    archetypes.required("reformer", Type::block(archetype));
    rules.required("archetypes", Type::block(std::move(archetypes)));
    ObjectSchema level("difficulty_level");
    level.required("ap_mult_pm", number());
    level.required("noise_pm", number());
    level.required("aggression_pm", number());
    level.required("player_bias_pm", number());
    level.required("forgiveness_pm", number());
    level.required("coalition_pm", number());
    ObjectSchema difficulty("difficulty");
    difficulty.required("friendly", Type::block(level));
    difficulty.required("realistic", Type::block(level));
    difficulty.required("ruthless", Type::block(level));
    rules.required("difficulty", Type::block(std::move(difficulty)));
    ObjectSchema signature("event_goal_signature");
    signature.required("effect", Type::identifier());
    signature.required("goal",
                       Type::enumeration({"gain_office",
                                          "secure_line",
                                          "grow_wealth",
                                          "gain_favor",
                                          "protect_self",
                                          "prepare_plot",
                                          "keep_order"}));
    signature.required("weight_pm", number());
    rules.required("wealth_bands", Type::list(Type::integer()));
    rules.required("event_goal_signatures", Type::list(Type::block(std::move(signature))));
    rules.required("archetype_follows_head", Type::boolean());
    set.add(std::move(rules));
    ObjectSchema agent("ai_agent");
    agent.intern_last().describe_kind(
        "Назначение агента роду сценария; окончательный состав M1 выбирается в E30-T08.");
    agent.required("dynasty", Type::reference("dynasty"));
    agent.required(
        "archetype",
        Type::enumeration(
            {"ambitious", "loyalist", "steward", "schemer", "acquisitive", "pious", "warrior", "reformer"}));
    agent.required("mode", Type::enumeration({"agent", "background"}));
    agent.optional("force_tier", Type::integer(1, 3));
    set.add(std::move(agent));
}
}  // namespace pat::data
