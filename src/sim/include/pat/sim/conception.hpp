#pragma once

#include "pat/core/string_interner.hpp"
#include "pat/sim/demography.hpp"
#include "pat/sim/names.hpp"
#include "pat/sim/system.hpp"
#include "pat/sim/traits.hpp"

#include <memory>
#include <string_view>
#include <vector>

// Conception, pregnancy and birth (E7-T12, GDD 01 sections 3.3 and 3.4, ADR-0006 "Implementation S4").
//
// The system `dynasty.conception` runs every day and does two things:
//   * conception: on the monthly slot of each married, non-pregnant wife (`is_monthly_slot(id)`, days
//     1...28) it rolls the monthly chance of conception_permille() and, on success, emits
//     cmd::Conceive with the date of the labour (or of an early miscarriage);
//   * labour: for every pregnancy that has come due (World::take_due_labours) it decides EVERYTHING
//     random about the birth - outcome, death of the mother, sex, skills, traits, name, portrait,
//     house and branch - and emits one cmd::Birth, so that applying the command is plain bookkeeping.
//
// Randomness comes from named streams only, and every entity consumes a FIXED number of draws whatever
// the branch (epic E7 section 3 row 8):
//   demography.conception   3 per eligible wife: conception, miscarriage, month of the miscarriage
//   demography.birth        3 per labour: sex, stillbirth, death of the mother
//   demography.genetics     5 skills + one per heritable trait of the catalogue, per labour
//   demography.names        2 per labour (given name, portrait) when name pools are configured
// A labour whose pregnancy was to end in a miscarriage draws the same numbers (and the unused ones
// are dropped), so that the sequence does not depend on the branch.
//
// Everything the system needs besides the world is passed in BirthContent and copied; nothing is kept
// between days except that content (the state is the World: the pregnancy table).

namespace pat::sim {

inline constexpr std::string_view kConceptionSystemName = "dynasty.conception";
inline constexpr std::string_view kConceptionStream = "demography.conception";
inline constexpr std::string_view kBirthStream = "demography.birth";
inline constexpr std::string_view kGeneticsStream = "demography.genetics";
inline constexpr std::string_view kNamesStream = "demography.names";

/// The content births need besides the rules.
struct BirthContent {
    DemographyRules rules = kDefaultDemography;
    /// Without a catalogue the traits of the parents neither change the chance of conception nor pass
    /// to the child (and the health of a mother is her column).
    std::shared_ptr<const TraitCatalog> traits;
    /// Without name pools newborns stay unnamed and without a portrait.
    std::shared_ptr<const NamePools> names;
    /// The id stored in the `name` column for each entry of `names->given_names`, in the same order
    /// (see intern_given_names). A pool and a list of different sizes count as no pools.
    std::vector<core::StringId> given_name_ids;
};

/// Interns the ids of the given names of the pools; the result is `BirthContent::given_name_ids`.
/// Setup time only (the interner is changed): the world refers to these ids afterwards.
[[nodiscard]] std::vector<core::StringId> intern_given_names(const NamePools& pools,
                                                             core::StringInterner& interner);

/// Rules must pass demography validation; the content is copied into the system.
[[nodiscard]] std::unique_ptr<System> make_conception_system(BirthContent content);

/// The same day of the month `months` later (a day past the end of that month becomes its last day).
[[nodiscard]] Date date_plus_months(Date date, int months) noexcept;

}  // namespace pat::sim
