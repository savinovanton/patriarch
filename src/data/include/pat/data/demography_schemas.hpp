#pragma once

#include "pat/data/schema.hpp"

#include <string_view>

// Schema of the demography balance (E7-T07, docs/gdd/01-dynasty.md section 3, docs/dev/data-reference.md).
// It describes data/balance/demography.pd and is registered by content_schemas(). The module depends on
// core only: turning the loaded object into pat::sim::DemographyRules is the job of the pat::content
// module (src/content), which sees both.
//
// The value corridors written here mirror pat::sim::validate(const DemographyRules&) (demography.cpp)
// so that most mistakes are reported with file:line:column by the schema (PD104); validate() stays the
// final check of the assembled rules (cross-field rules such as "ages of the stages increase").

namespace pat::data {

/// Name of the kind. The content must hold exactly one object of it; a mod replaces it whole by
/// repeating the same id.
inline constexpr std::string_view kDemographyRulesKind = "demography_rules";

/// Registers demography_rules in `set`.
void add_demography_schemas(SchemaSet& set);

}  // namespace pat::data
