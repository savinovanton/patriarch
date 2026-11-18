#include "pat/sim/crisis_rules.hpp"

#include "pat/sim/rules_check.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

namespace pat::sim::crisis {

namespace {

using rules_check::fail;
using Check = rules_check::Check<CrisisRulesError>;

Check in_range(std::string_view name, std::int64_t value, std::int64_t lo, std::int64_t hi) {
    return rules_check::in_range<CrisisRulesError>(name, value, lo, hi);
}

Check fail_at(std::string message, std::string field) {
    return fail<CrisisRulesError>(std::move(message), std::move(field));
}

std::vector<ProjectDef> default_projects() {
    ProjectDef war;
    war.slot = static_cast<std::uint8_t>(ProjectKind::War);
    war.id = "war";
    war.name_key = "crisis.project.war.name";
    war.desc_key = "crisis.project.war.desc";
    war.level_min = 0;
    war.level_max = 3;
    war.thresholds = {20, 45, 70};
    war.response_pivot = 50;
    war.response_k_pm = -800;
    war.effects = {{"war_fatigue_x100", {0, 60, 140, 260}},
                   {"recruit_quota_mult", {0, 1, 2, 3}},
                   {"army_upkeep_kop", {0, 15'000, 35'000, 60'000}}};

    ProjectDef taxes;
    taxes.slot = static_cast<std::uint8_t>(ProjectKind::Taxes);
    taxes.id = "taxes";
    taxes.name_key = "crisis.project.taxes.name";
    taxes.desc_key = "crisis.project.taxes.desc";
    taxes.level_min = -2;
    taxes.level_max = 3;
    taxes.thresholds = {-40, -15, 15, 40, 65};
    taxes.response_pivot = 55;
    taxes.response_k_pm = -2'000;
    taxes.effects = {{"per_weight_kop", {0, 40, 100, 160, 230, 320}}};

    ProjectDef purges;
    purges.slot = static_cast<std::uint8_t>(ProjectKind::Purges);
    purges.id = "purges";
    purges.name_key = "crisis.project.purges.name";
    purges.desc_key = "crisis.project.purges.desc";
    purges.level_min = 0;
    purges.level_max = 3;
    purges.thresholds = {25, 50, 75};
    purges.response_pivot = 50;
    purges.response_k_pm = 500;
    purges.effects = {{"purge_heat_x100", {0, 100, 250, 500}},
                      {"court_upkeep_kop", {0, 8'000, 20'000, 40'000}},
                      {"punish_tier", {0, 0, 1, 2}}};
    return {std::move(war), std::move(taxes), std::move(purges)};
}

Check check_balance(const CrisisBalance& b) {
#define PAT_CRISIS_FIELD_CHECK(group, type, name, def, lo, hi)                                               \
    PAT_RULES_CHECK(in_range(#group "." #name, b.group##_##name, lo, hi));
    PAT_CRISIS_BALANCE_FIELDS(PAT_CRISIS_FIELD_CHECK)
#undef PAT_CRISIS_FIELD_CHECK

    const auto above = [](std::int64_t high,
                          std::int64_t low,
                          std::string_view high_name,
                          std::string_view low_name) -> Check {
        if (high <= low) {
            return fail_at(std::format("{} must be above {}", high_name, low_name), std::string(high_name));
        }
        return Check::ok();
    };
    const auto weights = CrisisRules{b, {}, {}}.component_weights();
    std::int64_t sum = 0;
    for (const auto weight : weights) {
        sum += weight;
    }
    if (sum != 1'000) {
        return fail_at(std::format("component weights add up to {}, expected 1000", sum),
                       "components.weight_fiscal");
    }
    PAT_RULES_CHECK(above(b.bands_order_min, b.bands_tension_min, "bands.order_min", "bands.tension_min"));
    PAT_RULES_CHECK(above(b.bands_tension_min, b.bands_crisis_min, "bands.tension_min", "bands.crisis_min"));
    PAT_RULES_CHECK(above(b.phases_p1_exit, b.phases_p1_enter, "phases.p1_exit", "phases.p1_enter"));
    PAT_RULES_CHECK(
        above(b.phases_p2_susp_enter, b.phases_p2_susp_exit, "phases.p2_susp_enter", "phases.p2_susp_exit"));
    PAT_RULES_CHECK(above(b.phases_p3_exit, b.phases_p3_enter, "phases.p3_exit", "phases.p3_enter"));
    PAT_RULES_CHECK(above(b.phases_p1_enter, b.phases_p3_enter, "phases.p1_enter", "phases.p3_enter"));
    PAT_RULES_CHECK(above(b.emperor_ill_exit, b.emperor_ill_enter, "emperor.ill_exit", "emperor.ill_enter"));
    PAT_RULES_CHECK(
        above(b.emperor_heavy_exit, b.emperor_heavy_enter, "emperor.heavy_exit", "emperor.heavy_enter"));
    PAT_RULES_CHECK(
        above(b.emperor_ill_enter, b.emperor_heavy_enter, "emperor.ill_enter", "emperor.heavy_enter"));
    PAT_RULES_CHECK(
        above(b.suspicion_band_mid, b.suspicion_band_low, "suspicion.band_mid", "suspicion.band_low"));
    PAT_RULES_CHECK(
        above(b.suspicion_band_high, b.suspicion_band_mid, "suspicion.band_high", "suspicion.band_mid"));
    PAT_RULES_CHECK(above(b.projects_taxes_need_max,
                          b.projects_taxes_need_min,
                          "projects.taxes_need_max",
                          "projects.taxes_need_min"));
    if (b.suspicion_confirm_min > b.suspicion_confirm_max) {
        return fail_at("suspicion.confirm_min must not exceed suspicion.confirm_max",
                       "suspicion.confirm_min");
    }
    if (b.suspicion_false_expose_min > b.suspicion_false_expose_max) {
        return fail_at("suspicion.false_expose_min must not exceed suspicion.false_expose_max",
                       "suspicion.false_expose_min");
    }
    if (b.plot_strength_lo_pm > b.plot_strength_hi_pm) {
        return fail_at("plot.strength_lo_pm must not exceed plot.strength_hi_pm", "plot.strength_lo_pm");
    }
    if (b.coup_success_min > b.coup_success_max) {
        return fail_at("coup.success_min must not exceed coup.success_max", "coup.success_min");
    }
    if (b.coup_partial_min > b.coup_partial_max) {
        return fail_at("coup.partial_min must not exceed coup.partial_max", "coup.partial_min");
    }
    for (std::size_t i = 0; i < b.sigmoid.size(); ++i) {
        const auto name = std::format("sigmoid[{}]", i);
        PAT_RULES_CHECK(in_range(name, b.sigmoid[i], 0, 1'000));
        if (i > 0 && b.sigmoid[i] < b.sigmoid[i - 1]) {
            return fail_at(
                std::format("the sigmoid must not decrease: {} < {}", b.sigmoid[i], b.sigmoid[i - 1]), name);
        }
        if (b.sigmoid[i] + b.sigmoid[b.sigmoid.size() - 1 - i] != 1'000) {
            return fail_at(
                std::format("the sigmoid must be symmetric: sigmoid[{}] + sigmoid[{}] must be 1000",
                            i,
                            b.sigmoid.size() - 1 - i),
                name);
        }
    }
    return Check::ok();
}

Check check_claims(const ClaimRules& c) {
#define PAT_CLAIM_FIELD_CHECK(group, type, name, def, lo, hi)                                                \
    PAT_RULES_CHECK(in_range("claim_rules " #group "." #name, c.group##_##name, lo, hi));
    PAT_CLAIM_RULE_FIELDS(PAT_CLAIM_FIELD_CHECK)
#undef PAT_CLAIM_FIELD_CHECK
    for (std::size_t i = 0; i < c.blood.size(); ++i) {
        const auto name = std::format("claim_rules blood[{}]", i);
        PAT_RULES_CHECK(in_range(name, c.blood[i], 0, 100));
        if (i > 0 && c.blood[i] > c.blood[i - 1]) {
            return fail_at("blood terms must not grow with the kinship degree", name);
        }
    }
    for (std::size_t i = 0; i < c.legal.size(); ++i) {
        const auto name = std::format("claim_rules legal[{}]", i);
        PAT_RULES_CHECK(in_range(name, c.legal[i], 0, 100));
        if (i > 0 && c.legal[i] > c.legal[i - 1]) {
            return fail_at("legal terms must not grow with the place in the order", name);
        }
    }
    return Check::ok();
}

std::string project_path(const ProjectDef& p, std::string_view field) {
    return std::format("emperor_project {}.{}", p.id, field);
}

Check check_project(const ProjectDef& p) {
    if (p.id.empty()) {
        return fail_at("a project needs an id", std::format("emperor_project {}", p.slot));
    }
    PAT_RULES_CHECK(in_range(project_path(p, "slot"), p.slot, 1, kProjectCount));
    PAT_RULES_CHECK(in_range(project_path(p, "level_min"), p.level_min, -4, 0));
    PAT_RULES_CHECK(in_range(project_path(p, "level_max"), p.level_max, 1, 7));
    const auto levels = static_cast<std::size_t>(p.level_max - p.level_min + 1);
    if (levels > kMaxProjectLevels) {
        return fail_at(std::format("a project has at most {} levels", kMaxProjectLevels),
                       project_path(p, "level_max"));
    }
    if (p.thresholds.size() + 1 != levels) {
        return fail_at(std::format("expected {} thresholds for levels {}...{}, found {}",
                                   levels - 1,
                                   p.level_min,
                                   p.level_max,
                                   p.thresholds.size()),
                       project_path(p, "thresholds"));
    }
    for (std::size_t i = 0; i < p.thresholds.size(); ++i) {
        const auto name = project_path(p, std::format("thresholds[{}]", i));
        PAT_RULES_CHECK(in_range(name, p.thresholds[i], -1'000, 1'000));
        if (i > 0 && p.thresholds[i] <= p.thresholds[i - 1]) {
            return fail_at("thresholds must ascend", name);
        }
    }
    PAT_RULES_CHECK(in_range(project_path(p, "response_pivot"), p.response_pivot, 0, 100));
    PAT_RULES_CHECK(in_range(project_path(p, "response_k_pm"), p.response_k_pm, -10'000, 10'000));
    const auto& required = required_project_effects(static_cast<ProjectKind>(p.slot));
    for (const std::string_view name : required) {
        if (p.find_effect(name) == nullptr) {
            return fail_at(std::format("project '{}' needs the effect '{}'", p.id, name),
                           project_path(p, "effects"));
        }
    }
    for (const ProjectEffect& effect : p.effects) {
        const auto effect_path = project_path(p, "effects." + effect.name);
        if (std::ranges::find(required, effect.name) == required.end()) {
            return fail_at(std::format("project '{}' has no effect named '{}'", p.id, effect.name),
                           effect_path);
        }
        if (effect.per_level.size() != levels) {
            return fail_at(std::format("effect '{}' needs one number per level ({}), found {}",
                                       effect.name,
                                       levels,
                                       effect.per_level.size()),
                           effect_path);
        }
        for (std::size_t i = 0; i < effect.per_level.size(); ++i) {
            PAT_RULES_CHECK(
                in_range(effect_path + std::format("[{}]", i), effect.per_level[i], 0, 1'000'000'000));
        }
    }
    return Check::ok();
}

Check check_projects(const std::vector<ProjectDef>& projects) {
    if (projects.size() != kProjectCount) {
        return fail_at(std::format("expected {} emperor_project objects (war, taxes, purges), found {}",
                                   kProjectCount,
                                   projects.size()),
                       "emperor_project");
    }
    for (std::size_t i = 0; i < projects.size(); ++i) {
        if (projects[i].slot != i + 1) {
            return fail_at(std::format("project slots must be 1, 2, 3 in order; found {} at position {}",
                                       projects[i].slot,
                                       i + 1),
                           project_path(projects[i], "slot"));
        }
        PAT_RULES_CHECK(check_project(projects[i]));
    }
    return Check::ok();
}

}  // namespace

const ProjectEffect* ProjectDef::find_effect(std::string_view name) const noexcept {
    const auto it = std::ranges::find(effects, name, &ProjectEffect::name);
    return it != effects.end() ? &*it : nullptr;
}

const std::vector<std::string_view>& required_project_effects(ProjectKind kind) {
    static const std::vector<std::string_view> kWar{
        "war_fatigue_x100", "recruit_quota_mult", "army_upkeep_kop"};
    static const std::vector<std::string_view> kTaxes{"per_weight_kop"};
    static const std::vector<std::string_view> kPurges{"purge_heat_x100", "court_upkeep_kop", "punish_tier"};
    static const std::vector<std::string_view> kNone;
    switch (kind) {
        case ProjectKind::War:
            return kWar;
        case ProjectKind::Taxes:
            return kTaxes;
        case ProjectKind::Purges:
            return kPurges;
    }
    return kNone;
}

CrisisRules CrisisRules::defaults() {
    CrisisRules rules;
    rules.projects = default_projects();
    return rules;
}

const ProjectDef* CrisisRules::find_project(ProjectKind kind) const noexcept {
    const auto it = std::ranges::find(projects, static_cast<std::uint8_t>(kind), &ProjectDef::slot);
    return it != projects.end() ? &*it : nullptr;
}

std::array<std::int32_t, kComponentCount> CrisisRules::component_weights() const noexcept {
    return {balance.components_weight_fiscal,
            balance.components_weight_war,
            balance.components_weight_recruit,
            balance.components_weight_mood,
            balance.components_weight_court,
            balance.components_weight_church,
            balance.components_weight_clarity,
            balance.components_weight_authority};
}

core::Result<void, CrisisRulesError> validate(const CrisisRules& rules) {
    PAT_RULES_CHECK(check_balance(rules.balance));
    PAT_RULES_CHECK(check_claims(rules.claims));
    return check_projects(rules.projects);
}

}  // namespace pat::sim::crisis
