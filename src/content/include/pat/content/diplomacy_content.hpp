#pragma once
#include "pat/content/load_errors.hpp"
#include "pat/sim/diplomacy/rules.hpp"

#include <span>
namespace pat::content {
struct DiplomacyContent {
    sim::diplomacy::DiplomacyRules rules;
    std::uint64_t rules_hash = 0;
};
using DiplomacyRulesResult = core::Result<sim::diplomacy::DiplomacyRules, std::vector<data::LoadError>>;
using DiplomacyContentResult = core::Result<DiplomacyContent, std::vector<data::LoadError>>;
// Requires an error-free registry. Trait hints reference existing traits but never mutate them.
[[nodiscard]] DiplomacyRulesResult build_diplomacy_rules(const data::ContentRegistry& registry);
[[nodiscard]] DiplomacyContentResult
load_diplomacy_content(std::span<const data::ContentSource* const> roots);
[[nodiscard]] DiplomacyContentResult load_diplomacy_content(const data::ContentSource& root);
}  // namespace pat::content
