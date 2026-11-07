#pragma once

#include "pat/data/schema.hpp"

#include <string_view>

// Schemas of the economy content kinds (E9-T18, docs/dev/economy-model.md sections 11 and 15,
// docs/dev/data-reference.md). They describe the files in data/economy/ and are registered by
// content_schemas(). The module depends on core only: turning the loaded objects into
// pat::sim::economy::EconomyRules is the job of the pat::content module (src/content), which sees both.
//
// The value corridors written here mirror pat::sim::economy::validate() (rules.cpp) so that most
// mistakes are reported with file:line:column by the schema (PD104); validate() stays the final
// check of the assembled rules (cross-field rules such as "anarchy < low control").

namespace pat::data {

/// Names of the economy kinds. The loader of the economy content (pat::content) reads these tables.
inline constexpr std::string_view kEconomyRulesKind = "economy_rules";
inline constexpr std::string_view kGoodKind = "good";
inline constexpr std::string_view kPopulationGroupKind = "population_group";
inline constexpr std::string_view kProvinceKindKind = "province_kind";
inline constexpr std::string_view kTaxLevelKind = "tax_level";
inline constexpr std::string_view kRecruitLevelKind = "recruit_level";
inline constexpr std::string_view kBuildingKind = "building";

/// Registers economy_rules, good, population_group, province_kind, tax_level, recruit_level and
/// building in `set`.
void add_economy_schemas(SchemaSet& set);

}  // namespace pat::data
