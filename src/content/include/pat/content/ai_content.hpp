#pragma once
#include "pat/content/load_errors.hpp"
#include "pat/data/content_loader.hpp"
#include "pat/sim/ai/rules.hpp"
#include "pat/sim/events/catalog.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace pat::content {
// Store scenario keys, not World ids: scenario assembly resolves keys when AI is enabled (T19/E30).
enum class AiAgentMode : std::uint8_t { Agent = 3, Background = 4 };
struct AiAgentDefinition {
    std::string dynasty;
    sim::ai::Archetype archetype = sim::ai::Archetype::Ambitious;
    AiAgentMode mode = AiAgentMode::Agent;
    std::uint8_t force_tier = 0;  // 0: automatic; 1..3: scenario override.
    bool operator==(const AiAgentDefinition&) const = default;
};
using AiRulesResult = core::Result<sim::ai::AiRules, std::vector<data::LoadError>>;
using AiAgentsResult = core::Result<std::vector<AiAgentDefinition>, std::vector<data::LoadError>>;
struct AiContent {
    sim::ai::AiRules rules;
    std::vector<AiAgentDefinition> agents;
};
using AiContentResult = core::Result<AiContent, std::vector<data::LoadError>>;
// Registry must have passed schema/reference validation. Domain effects are resolved through the
// actual E11 catalog; with nullptr only an empty signature table is accepted (T16 adds catalogs).
[[nodiscard]] AiRulesResult build_ai_rules(const data::ContentRegistry& registry,
                                           const sim::events::Catalog* catalog = nullptr);
// player_dynasties comes from the scenario/menu, never from a second player flag in ai_agents.pd.
[[nodiscard]] AiAgentsResult build_ai_agents(const data::ContentRegistry& registry,
                                             std::span<const std::string_view> player_dynasties = {});
[[nodiscard]] AiContentResult load_ai_content(std::span<const data::ContentSource* const> roots,
                                              std::span<const std::string_view> player_dynasties = {},
                                              const sim::events::Catalog* catalog = nullptr);
[[nodiscard]] AiContentResult load_ai_content(const data::ContentSource& root,
                                              std::span<const std::string_view> player_dynasties = {},
                                              const sim::events::Catalog* catalog = nullptr);
}  // namespace pat::content
