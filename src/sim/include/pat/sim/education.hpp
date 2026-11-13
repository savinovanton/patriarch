#pragma once

#include "pat/sim/demography.hpp"
#include "pat/sim/system.hpp"
#include "pat/sim/traits.hpp"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

// Education and coming of age (E7-T13, GDD 01 section 5, US-7.8).
//
// A child gets a FOCUS (one of the five skills) and may get a TUTOR with the command cmd::SetEducation.
// The system `dynasty.education` runs every day and does three things:
//   * progress: on the monthly slot of each living child from the start of adolescence with a focus, the
//     education level (0...100) grows by education_monthly_gain(): a point a month, more with a tutor of
//     a high skill in the focus skill. A tutor who died is dropped;
//   * the automatic choice: a child of a house that is NOT one of `player_dynasties` who has no focus gets
//     one on the monthly slot (the skill in which the child is best; the best adult of the house in that
//     skill as the tutor) by a SetEducation command of the system. Houses of the player choose themselves:
//     a child they neglect grows up without an education;
//   * coming of age: on the 16th birthday (World::take_due_adulthoods, each living character exactly once)
//     it decides what the education brings, from the level and the tutor's skill (education_outcome), and
//     emits one cmd::ComeOfAge: the skill bonus for the focus skill and, from trait_min_level, the
//     education trait of the focus (education_trait_for). A child without a focus still comes of age, with
//     no bonus and no trait.
//
// There is no randomness at all: nothing is drawn from a stream. The state is the education columns of the
// characters; the queue of birthdays is derived by the World. Nothing is kept in the system but its content.

namespace pat::sim {

inline constexpr std::string_view kEducationSystemName = "dynasty.education";

/// What the education system needs besides the world.
struct EducationContent {
    DemographyRules rules = kDefaultDemography;
    /// Without a catalogue the education brings no trait, and skills of tutors are their stored columns.
    std::shared_ptr<const TraitCatalog> traits;
    /// The houses whose children are educated by their players only; every other house is automatic.
    std::vector<DynastyId> player_dynasties;
};

/// The education trait of a focus skill: of the traits of the category Education, the one with the
/// largest positive modifier for that skill (the smallest id on a tie); 0 when the catalogue has none.
[[nodiscard]] std::uint16_t education_trait_for(const TraitCatalog& catalog, Skill skill) noexcept;

/// Rules must pass demography validation; the content is copied into the system.
[[nodiscard]] std::unique_ptr<System> make_education_system(EducationContent content);

}  // namespace pat::sim
