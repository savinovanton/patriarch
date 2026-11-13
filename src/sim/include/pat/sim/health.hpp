#pragma once

#include "pat/sim/demography.hpp"
#include "pat/sim/system.hpp"
#include "pat/sim/traits.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

// Health, stress and fertility (E7-T11, GDD 01 sections 2.1 and 3, epic E7 section 3 row 8).
//
// The system `dynasty.health` runs every day and handles, on the monthly slot of each living character
// (`is_monthly_slot(id, date)`, days 1...28), one monthly step:
//   * the stored health drifts by life stage (monthly_health_change): growth in childhood, nothing in the
//     prime, loss in maturity, fast loss in old age; an unwell body and a strained mind cost more; a small
//     random swing comes from the stream `demography.health` (ONE draw per character per month, always);
//   * the stored stress gains from illness and from leading a house, and a share of it fades
//     (monthly_stress_change);
//   * the stored fertility is derived from the age and the effective health (derived_fertility);
//   * when the EFFECTIVE stress (traits included) rises to a higher StressLevel, a note about it goes to
//     the chronicle of the dynasty (kNoteStressStrained / kNoteStressBreaking). This is the hook for the
//     events of E11, which read the level from the snapshot; there are no illnesses as events yet.
//
// The system changes the world through the World API (like dynasty.standing): the state is the columns of
// the characters and the chronicle, nothing is kept in the system itself, so saves and replays need no
// extra data. Traits are optional: without a catalogue the effective values are the stored ones.

namespace pat::sim {

inline constexpr std::string_view kHealthSystemName = "dynasty.health";
inline constexpr std::string_view kHealthStream = "demography.health";

/// Codes of the chronicle notes written by the health system (`ChronicleEntry::param`, kind Note, `a` is
/// the character). Codes below 100 are left to the editorial notes of E7-T16.
inline constexpr std::int32_t kNoteStressStrained = 101;
inline constexpr std::int32_t kNoteStressBreaking = 102;

/// Rules must pass demography validation and are copied into the system.
[[nodiscard]] std::unique_ptr<System> make_health_system(const DemographyRules& rules,
                                                         std::shared_ptr<const TraitCatalog> traits = {});

}  // namespace pat::sim
