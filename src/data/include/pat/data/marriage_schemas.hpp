#pragma once

#include "pat/data/schema.hpp"

#include <string_view>

namespace pat::data {

inline constexpr std::string_view kMarriageRulesKind = "marriage_rules";

/// Registers the strict balance schema; assembly and cross-field checks belong to content/sim.
void add_marriage_schemas(SchemaSet& set);

}  // namespace pat::data
