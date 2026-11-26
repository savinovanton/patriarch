#pragma once
#include "pat/data/schema.hpp"

#include <array>
#include <string_view>
namespace pat::data {
inline constexpr std::string_view kArmyRulesKind = "army_rules";
inline constexpr std::string_view kMilitarySetupKind = "military_setup";
// These kinds are opt-in and excluded from M1 simulation compatibility until E16-T20.
inline constexpr std::array<std::string_view, 2> kArmyKinds{kArmyRulesKind, kMilitarySetupKind};
void add_army_schemas(SchemaSet& set);
}  // namespace pat::data
