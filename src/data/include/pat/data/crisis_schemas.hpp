#pragma once

#include "pat/data/schema.hpp"

#include <array>
#include <string_view>
#include <vector>

// Schemas of the crisis content kinds (E12-T04, docs/dev/throne-crisis-model.md section 11,
// docs/dev/data-reference.md). They describe data/balance/crisis.pd and data/crisis/projects.pd and are
// registered by content_schemas(). The module depends on core only: turning the loaded objects into
// pat::sim::crisis::CrisisRules is the job of pat::content (src/content/crisis_content.cpp).
//
// Only representation limits are written here; the value corridors and the cross-field rules belong to
// pat::sim::crisis::validate(const CrisisRules&), which the builder runs on the assembled rules.

namespace pat::data {

inline constexpr std::string_view kCrisisBalanceKind = "crisis_balance";
inline constexpr std::string_view kClaimRulesKind = "claim_rules";
inline constexpr std::string_view kEmperorProjectKind = "emperor_project";

/// The kinds of the crisis. They stay out of the simulation compatibility hash of M1 until the crisis
/// systems are installed in the scenario (E12-T22, throne-crisis-model.md section 13).
inline constexpr std::array<std::string_view, 3> kCrisisKinds{
    kCrisisBalanceKind, kClaimRulesKind, kEmperorProjectKind};

/// One number of a balance object: its block, its key and whether it is a 64-bit amount.
struct CrisisBalanceField {
    std::string_view group;
    std::string_view name;
    bool wide;
    std::string_view description;
};

/// Every scalar field of `crisis_balance` in schema order (a test pins it to PAT_CRISIS_BALANCE_FIELDS).
[[nodiscard]] const std::vector<CrisisBalanceField>& crisis_balance_fields();
/// Every scalar field of `claim_rules` in schema order (a test pins it to PAT_CLAIM_RULE_FIELDS).
[[nodiscard]] const std::vector<CrisisBalanceField>& claim_rule_fields();

/// Registers crisis_balance, claim_rules and emperor_project in `set`.
void add_crisis_schemas(SchemaSet& set);

}  // namespace pat::data
