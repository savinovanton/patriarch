#pragma once

#include "pat/data/schema.hpp"

#include <array>
#include <string_view>
#include <vector>

// Schemas of the court content kinds (E10-T04, docs/dev/court-model.md section 11,
// docs/dev/data-reference.md). They describe data/court/*.pd and data/balance/court.pd and are
// registered by content_schemas(). The module depends on core only: turning the loaded objects into
// pat::sim::CourtRules is the job of pat::content (src/content/court_content.cpp).
//
// Only representation limits are written here; the value corridors and the cross-field rules belong
// to pat::sim::validate(const CourtRules&), which the builder runs on the assembled rules.

namespace pat::data {

inline constexpr std::string_view kOfficeKind = "office";
inline constexpr std::string_view kServantRoleKind = "servant_role";
inline constexpr std::string_view kCourtPartyKind = "court_party";
inline constexpr std::string_view kCourtBalanceKind = "court_balance";

/// The kinds of the court. They stay out of the simulation compatibility hash of M1 until the court
/// systems are installed in the scenario (E10-T20, court-model.md section 13.2).
inline constexpr std::array<std::string_view, 4> kCourtKinds{
    kOfficeKind, kServantRoleKind, kCourtPartyKind, kCourtBalanceKind};

/// One number of the balance object: its block, its key and whether it is a 64-bit amount.
struct CourtBalanceField {
    std::string_view group;
    std::string_view name;
    bool wide;
    std::string_view description;
};

/// Every field of `court_balance` in schema order (a test pins it to PAT_COURT_BALANCE_FIELDS).
[[nodiscard]] const std::vector<CourtBalanceField>& court_balance_fields();

/// Registers office, servant_role, court_party and court_balance in `set`.
void add_court_schemas(SchemaSet& set);

}  // namespace pat::data
