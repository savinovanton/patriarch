#pragma once

#include "pat/sim/date.hpp"
#include "pat/sim/entity_ids.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// State of the dynasty resource "influence" (docs/dev/influence-model.md section 4, declared by E10-T02):
// a sparse balance per dynasty and a global ring of the last changes. World owns every change
// (World::add_influence); the income, the cap and the prices are rules of E7-T40 and later; persistence
// is E10-T03 (section 10, hash part 40).

namespace pat::sim {

/// Absolute ceiling of a balance (protection against overflow); the game cap is a rule (section 5.3).
inline constexpr std::int32_t kMaxInfluenceX100 = 30000;
inline constexpr std::size_t kInfluenceLogSize = 64;

/// Why the balance changed; values are stable forever (E7 1...99, E10 300...399, E11 400...499).
enum class InfluenceReason : std::uint16_t {
    Monthly = 1,
    Start = 2,
    MediationSoften = 10,
    MediationReconcile = 11,
    GiftToMember = 12,
    Scenario = 90,
    PetitionPush = 301,
    PortfolioOrder = 302,
    Event = 400,
};

[[nodiscard]] constexpr bool is_known_influence_reason(std::uint16_t value) noexcept {
    switch (static_cast<InfluenceReason>(value)) {
        case InfluenceReason::Monthly:
        case InfluenceReason::Start:
        case InfluenceReason::MediationSoften:
        case InfluenceReason::MediationReconcile:
        case InfluenceReason::GiftToMember:
        case InfluenceReason::Scenario:
        case InfluenceReason::PetitionPush:
        case InfluenceReason::PortfolioOrder:
        case InfluenceReason::Event:
            return true;
    }
    return false;
}

/// The balance of a dynasty; a row exists while the balance is positive (1...kMaxInfluenceX100).
struct InfluenceRow {
    DynastyId dynasty;
    std::int32_t balance_x100 = 0;

    [[nodiscard]] bool operator==(const InfluenceRow&) const = default;
};

/// A logged change: the applied delta (after clamping). The monthly income is never logged.
struct InfluenceLogEntry {
    Date date = kNoDate;
    DynastyId dynasty;
    std::int32_t delta_x100 = 0;
    InfluenceReason reason = InfluenceReason::Start;

    [[nodiscard]] bool operator==(const InfluenceLogEntry&) const = default;
};

struct InfluenceState {
    std::vector<InfluenceRow> rows;      ///< ascending dynasty
    std::vector<InfluenceLogEntry> log;  ///< ring, oldest first, dates do not decrease

    [[nodiscard]] bool operator==(const InfluenceState&) const = default;
};

}  // namespace pat::sim
