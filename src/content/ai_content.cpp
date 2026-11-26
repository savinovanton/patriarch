#include "pat/content/ai_content.hpp"

#include "pat/data/content_schemas.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <set>
#include <tuple>
#include <utility>

#include "typed_accessors.hpp"
namespace pat::content {
AiRulesResult build_ai_rules(const data::ContentRegistry& registry, const sim::events::Catalog* catalog) {
    ErrorSink errors(registry);
    const auto* object = errors.single_object("ai_rules");
    if (object == nullptr)
        return AiRulesResult::err(errors.take());
    if (object->object.id != "main")
        errors.fail(*object, object->object.pos, "ai_rules id must be 'main'");
    sim::ai::AiRules rules;
    const auto& body = object->object.body;
    if (const auto* group = detail::block_of(body, "budget")) {
#define PAT_AI_READ(name, def, lo, hi) rules.budget.name = detail::i32_of(*group, #name);
        PAT_AI_BUDGET_FIELDS(PAT_AI_READ)
#undef PAT_AI_READ
    }
    if (const auto* group = detail::block_of(body, "schedule")) {
#define PAT_AI_READ(name, def, lo, hi) rules.schedule.name = detail::i32_of(*group, #name);
        PAT_AI_SCHEDULE_FIELDS(PAT_AI_READ)
#undef PAT_AI_READ
    }
    if (const auto* group = detail::block_of(body, "attention")) {
#define PAT_AI_READ(name, def, lo, hi) rules.attention.name = detail::i32_of(*group, #name);
        PAT_AI_ATTENTION_FIELDS(PAT_AI_READ)
#undef PAT_AI_READ
    }
    if (const auto* group = detail::block_of(body, "goals")) {
#define PAT_AI_READ(name, def, lo, hi) rules.goals.name = detail::i32_of(*group, #name);
        PAT_AI_GOALS_FIELDS(PAT_AI_READ)
#undef PAT_AI_READ
    }
    if (const auto* group = detail::block_of(body, "hysteresis")) {
#define PAT_AI_READ(name, def, lo, hi) rules.hysteresis.name = detail::i32_of(*group, #name);
        PAT_AI_HYSTERESIS_FIELDS(PAT_AI_READ)
#undef PAT_AI_READ
    }
    if (const auto* group = detail::block_of(body, "commitment")) {
#define PAT_AI_READ(name, def, lo, hi) rules.commitment.name = detail::i32_of(*group, #name);
        PAT_AI_COMMITMENT_FIELDS(PAT_AI_READ)
#undef PAT_AI_READ
    }
    if (const auto* group = detail::block_of(body, "cooldowns")) {
#define PAT_AI_READ(name, def, lo, hi) rules.cooldowns.name = detail::i32_of(*group, #name);
        PAT_AI_COOLDOWNS_FIELDS(PAT_AI_READ)
#undef PAT_AI_READ
    }
    if (const auto* group = detail::block_of(body, "archetype_change")) {
#define PAT_AI_READ(name, def, lo, hi) rules.archetype_change.name = detail::i32_of(*group, #name);
        PAT_AI_ARCHETYPE_CHANGE_FIELDS(PAT_AI_READ)
#undef PAT_AI_READ
    }
    if (const auto* group = detail::block_of(body, "command_costs")) {
#define PAT_AI_READ(name, def, lo, hi) rules.command_costs.name = detail::i32_of(*group, #name);
        PAT_AI_COMMAND_COSTS_FIELDS(PAT_AI_READ)
#undef PAT_AI_READ
    }
    if (const auto* block = detail::block_of(body, "archetypes")) {
        for (std::size_t i = 0; i < rules.archetypes.size(); ++i) {
            const auto* entry = detail::block_of(*block, sim::ai::kArchetypeKeys[i]);
            if (entry == nullptr)
                continue;  // Validated registries always have every named entry.
            auto& item = rules.archetypes[i];
            if (const auto* weights = detail::block_of(*entry, "weights"))
                for (std::size_t g = 0; g < item.weights.size(); ++g)
                    item.weights[g] = detail::i32_of(*weights, sim::ai::kGoalKeys[g]);
            item.risk_pm = detail::i32_of(*entry, "risk_pm");
            item.forgiveness_pm = detail::i32_of(*entry, "forgiveness_pm");
            item.gift_share_pm = detail::i32_of(*entry, "gift_share_pm");
            item.denounce_bias_pm = detail::i32_of(*entry, "denounce_bias_pm");
            item.faith_bias_pm = detail::i32_of(*entry, "faith_bias_pm");
            item.reform_bias_pm = detail::i32_of(*entry, "reform_bias_pm");
            item.front_eagerness_pm = detail::i32_of(*entry, "front_eagerness_pm");
            item.tax_bias = detail::i32_of(*entry, "tax_bias");
        }
    }
    if (const auto* block = detail::block_of(body, "difficulty")) {
        for (std::size_t i = 0; i < rules.difficulty.size(); ++i) {
            const auto* entry = detail::block_of(*block, sim::ai::kDifficultyKeys[i]);
            if (entry == nullptr)
                continue;
            auto& item = rules.difficulty[i];
            item.ap_mult_pm = detail::i32_of(*entry, "ap_mult_pm");
            item.noise_pm = detail::i32_of(*entry, "noise_pm");
            item.aggression_pm = detail::i32_of(*entry, "aggression_pm");
            item.player_bias_pm = detail::i32_of(*entry, "player_bias_pm");
            item.forgiveness_pm = detail::i32_of(*entry, "forgiveness_pm");
            item.coalition_pm = detail::i32_of(*entry, "coalition_pm");
        }
    }
    const auto* bands = detail::list_of(body, "wealth_bands");
    if (bands == nullptr || bands->size() != rules.wealth_bands.size()) {
        errors.fail(*object,
                    detail::field_pos(*object, "wealth_bands"),
                    "wealth_bands needs exactly four thresholds");
    } else {
        for (std::size_t i = 0; i < bands->size(); ++i)
            rules.wealth_bands[i] = *(*bands)[i].get_if<std::int64_t>();
    }
    if (const auto* value = detail::value_of<bool>(body, "archetype_follows_head"))
        rules.archetype_follows_head = *value;
    if (const auto* signatures = detail::list_of(body, "event_goal_signatures")) {
        for (const auto& value : *signatures) {
            const auto& entry = *value.get_if<data::TypedBlock>();
            sim::ai::EventGoalSignature signature;
            signature.effect = *detail::value_of<std::string>(entry, "effect");
            signature.goal =
                static_cast<sim::ai::GoalKind>(detail::value_of<data::EnumValue>(entry, "goal")->index + 1);
            signature.weight_pm = detail::i32_of(entry, "weight_pm");
            if (catalog == nullptr ||
                catalog->find(sim::events::NameSpace::Effect, signature.effect) == nullptr) {
                errors.fail(*object,
                            detail::key_pos(entry, "effect", value.pos),
                            std::format("unknown AI signature effect '{}'", signature.effect));
            }
            rules.event_goal_signatures.push_back(std::move(signature));
        }
        std::ranges::sort(rules.event_goal_signatures, [](const auto& a, const auto& b) {
            return std::tie(a.effect, a.goal) < std::tie(b.effect, b.goal);
        });
    }
    if (errors.empty()) {
        if (const auto valid = sim::ai::validate(rules); !valid)
            errors.fail(*object, detail::path_position(*object, valid.error().field), valid.error().message);
    }
    if (!errors.empty())
        return AiRulesResult::err(errors.take());
    return AiRulesResult::ok(std::move(rules));
}
AiAgentsResult build_ai_agents(const data::ContentRegistry& registry,
                               std::span<const std::string_view> player_dynasties) {
    ErrorSink errors(registry);
    std::vector<AiAgentDefinition> definitions;
    std::set<std::string> seen;
    if (const auto* table = registry.table("ai_agent")) {
        for (const auto& object : table->objects()) {
            const auto& ref = *detail::value_of<data::RefValue>(object.object.body, "dynasty");
            if (!seen.insert(ref.id).second)
                errors.fail(
                    object, detail::field_pos(object, "dynasty"), "a dynasty may have only one AI agent");
            if (std::ranges::find(player_dynasties, ref.id) != player_dynasties.end())
                errors.fail(object,
                            detail::field_pos(object, "dynasty"),
                            "a player dynasty must not be listed in ai_agents");
            definitions.push_back(
                {ref.id,
                 static_cast<sim::ai::Archetype>(detail::enum_index_of(object, "archetype") + 1),
                 detail::enum_index_of(object, "mode") == 0 ? AiAgentMode::Agent : AiAgentMode::Background,
                 static_cast<std::uint8_t>(detail::i32_of(object, "force_tier"))});
        }
    }
    if (!errors.empty())
        return AiAgentsResult::err(errors.take());
    std::ranges::sort(definitions, {}, &AiAgentDefinition::dynasty);
    return AiAgentsResult::ok(std::move(definitions));
}
AiContentResult load_ai_content(std::span<const data::ContentSource* const> roots,
                                std::span<const std::string_view> player_dynasties,
                                const sim::events::Catalog* catalog) {
    auto loaded = data::load_content(roots, data::content_schemas());
    if (!loaded.ok())
        return AiContentResult::err(std::move(loaded.errors));
    auto rules = build_ai_rules(loaded.registry, catalog);
    if (!rules)
        return AiContentResult::err(std::move(rules).error());
    auto agents = build_ai_agents(loaded.registry, player_dynasties);
    if (!agents)
        return AiContentResult::err(std::move(agents).error());
    return AiContentResult::ok({std::move(rules).value(), std::move(agents).value()});
}
AiContentResult load_ai_content(const data::ContentSource& root,
                                std::span<const std::string_view> player_dynasties,
                                const sim::events::Catalog* catalog) {
    const std::array<const data::ContentSource*, 1> roots{&root};
    return load_ai_content(roots, player_dynasties, catalog);
}
}  // namespace pat::content
