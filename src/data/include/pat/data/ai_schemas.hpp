#pragma once
#include "pat/data/schema.hpp"

#include <array>
#include <string_view>
namespace pat::data {
inline constexpr std::array<std::string_view, 2> kAiKinds{"ai_rules", "ai_agent"};
void add_ai_schemas(SchemaSet& set);
}  // namespace pat::data
