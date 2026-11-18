#pragma once

#include "pat/sim/crisis_rules.hpp"

#include <cstdint>
#include <string_view>

// Crisis presentation metadata (E12-T04, docs/dev/throne-crisis-model.md sections 8.1, 8.2, 9, 11): stable
// codes and the localisation keys of data/loc/ru/crisis.pd. The functions return an empty view for a code
// they do not know. Reason codes are written into the stability state and saves and never change meaning.

namespace pat::sim::crisis {

/// Which term of a component input dominates (`reason[i]` of the stability state); 0 is "no reason".
enum class StabilityReason : std::uint8_t {
    None = 0,
    TaxesHeavy = 1,
    Arrears = 2,
    Deficit = 3,
    WarFatigue = 4,
    RecruitHeavy = 5,
    RecruitShort = 6,
    EstatesUnhappy = 7,
    CourtOpposition = 8,
    KnownPlots = 9,
    PurgeHeat = 10,
    ChurchConflict = 11,
    ThroneVacant = 12,
    ThroneDisputed = 13,
    HeirAppointed = 14,
    HeirLawful = 15,
    HeirMinor = 16,
    NoHeirs = 17,
    RivalClaims = 18,
    EmperorAuthority = 19,
    Legacy = 20,
};
inline constexpr std::uint8_t kStabilityReasonCount = 20;

/// `crisis.component.<name>` of a stability component (the ninth, the legacy of unrest, has its own key).
[[nodiscard]] std::string_view component_key(Component component) noexcept;
[[nodiscard]] std::string_view legacy_component_key() noexcept;

/// `crisis.reason.<name>` of a stability reason code.
[[nodiscard]] std::string_view stability_reason_key(StabilityReason reason) noexcept;

/// `crisis.phase.<name>.name` and `.hint` of a phase 0...3 (smoulder, unrest, plots, coup_window).
[[nodiscard]] std::string_view phase_name_key(std::uint8_t phase) noexcept;
[[nodiscard]] std::string_view phase_hint_key(std::uint8_t phase) noexcept;

/// `crisis.band.<name>` of the stability band 0...3 (order, tension, crisis, collapse).
[[nodiscard]] std::string_view band_key(std::uint8_t band) noexcept;

}  // namespace pat::sim::crisis
