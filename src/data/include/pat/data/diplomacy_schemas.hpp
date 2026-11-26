#pragma once
#include "pat/data/schema.hpp"

#include <array>
#include <string_view>
namespace pat::data {
inline constexpr std::string_view kDiplomacyRulesKind = "diplomacy_balance";
inline constexpr std::string_view kTreatyKind = "treaty_kind";
inline constexpr std::array<std::string_view, 2> kDiplomacyKinds{kDiplomacyRulesKind, kTreatyKind};
void add_diplomacy_schemas(SchemaSet& set);
}  // namespace pat::data
