#pragma once

#include "pat/data/schema.hpp"

#include <string_view>

namespace pat::data {

/// Sorts after simulation definition kinds so rank ids do not shift the M1 world name ids.
inline constexpr std::string_view kHonorRankKind = "ui_honor_rank";
inline constexpr std::string_view kHoldingKind = "holding";
inline constexpr std::string_view kTradeNodeKind = "trade_node";
inline constexpr std::string_view kEconomyStartKind = "economy_start";

/// Adds the M1 definition kinds and the presentation-only honor ranks.
/// Cross-object constraints and map correspondence are checked by the content layer.
void add_m1_schemas(SchemaSet& schemas);

}  // namespace pat::data
