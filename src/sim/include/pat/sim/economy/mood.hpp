#pragma once

#include "pat/core/fixed.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/economy/types.hpp"
#include "pat/sim/entity_ids.hpp"

#include <array>
#include <cstdint>

// Mood of the population groups of a province (E9-T11, docs/dev/economy-model.md section 9 formula F8
// and section 7.3 step 5, GDD 03 section 4.2).
//
// Everything below the "province" line is a pure function of its arguments: integer-only, no random
// numbers, no World. The World is touched in exactly two places - `mood_input` reads one province into
// a plain struct and `apply_province_mood` writes the six moods back - so the formula can be tested,
// explained (Breakdown) and replaced without a world.
//
// The mood of a group (points, -100..100) tends to a target and moves toward it a part of the gap a
// month (`MoodRules::inertia_permille`, `approach` of administrator.hpp: truncated toward zero, at
// least one raw unit until the target is reached, so the mood arrives exactly):
//
//   target = base_mood[group]
//          - tax_pressure[tax level][group]                  the tax policy of the province
//          - recruit_pressure[recruit level][group]          the recruit policy of the province
//          - coercion                                        0 in M1 (corvee, forced labour: M2)
//          - religious conflict                              0 in M1 (E31)
//          + wealth_slope x (wealth - 100)                   wealth of THIS group
//          + admin_slope x (control - 50)                    the grip of the administrator
//          + events                                          hook of E11 (a slot of the event system)
//          clamped to -100..100
//
// The pressure of a policy level is positive for a heavy level and may be negative for a light one
// ("low tax" relieves the people); the slopes are per-mille of a point per point. The shortage of bread
// reaches the mood ONLY through the wealth term (E9-T07 decided so: the production step lowers wealth by
// the shortage, and counting it again here would count it twice); this module never reads the grain.
//
// A level index past the table is clamped to the last level, like the taxes do. A group with no
// people still has a mood (it is a column of the province, not of its people).

namespace pat::sim {
class World;
}

namespace pat::sim::economy {

/// Control at which the grip of the administrator neither raises nor lowers the mood (F8).
inline constexpr std::int32_t kNeutralControlPoints = 50;

/// Everything F8 reads about one province, as plain values.
struct MoodInput {
    std::array<core::Fixed32, kGroupCount> mood{};
    std::array<core::Fixed32, kGroupCount> wealth{};
    core::Fixed32 control = kDefaultControl;
    std::uint8_t tax_level = 0;      ///< index into `TaxRules::levels`
    std::uint8_t recruit_level = 0;  ///< index into `RecruitRules::levels`
    /// Hook of E11: the mood effect of events and decrees per group, points; nothing writes it yet.
    std::array<core::Fixed32, kGroupCount> events{};
};

/// One group after the month.
struct GroupMood {
    core::Fixed32 target;  ///< the mood the group tends to
    core::Fixed32 mood;    ///< the mood after the month
    [[nodiscard]] bool operator==(const GroupMood&) const = default;
};

struct ProvinceMood {
    std::array<GroupMood, kGroupCount> group{};
    [[nodiscard]] bool operator==(const ProvinceMood&) const = default;
};

// ---- the formula ---------------------------------------------------------------------------------

/// The pressure of the tax policy level on a group (points; clamped index), as the formula uses it.
[[nodiscard]] core::Fixed32 tax_mood_pressure(const TaxRules& tax, std::uint8_t level, Group group) noexcept;
/// The pressure of the recruit policy level on a group (points; clamped index).
[[nodiscard]] core::Fixed32
recruit_mood_pressure(const RecruitRules& recruit, std::uint8_t level, Group group) noexcept;

/// The mood a group tends to. Root of the breakdown: term::kMoodTarget (Points); children in the order
/// of the formula: kMoodBase, kMoodTaxPressure (negative for a heavy level), kMoodRecruitPressure,
/// kMoodCoercion and kMoodReligion (zero in M1, so left out), kMoodWealth and kMoodAdministrator
/// (`factor_permille` = the slope), kMoodEvents and kMoodLimit (the step the clamp to -100..100 made);
/// a term that is 0 is left out except the base. An unknown group gives 0.
[[nodiscard]] core::Fixed32 compute_group_target(const EconomyRules& rules,
                                                 const MoodInput& input,
                                                 Group group,
                                                 Breakdown* breakdown = nullptr);

/// F8: the target and one month of movement of a group. The result stays within -100..100 whatever
/// the input (a mood written out of range is pulled back into it).
[[nodiscard]] GroupMood compute_group_mood(const EconomyRules& rules,
                                           const MoodInput& input,
                                           Group group,
                                           Breakdown* breakdown = nullptr);

/// F8 for the six groups.
[[nodiscard]] ProvinceMood compute_province_mood(const EconomyRules& rules, const MoodInput& input);

// ---- province ------------------------------------------------------------------------------------

/// The input of a province as the World holds it; a default input for a world without economy or an
/// unknown province.
[[nodiscard]] MoodInput mood_input(const World& world, ProvinceId province);

/// Writes the six moods of a result into the columns of the province.
void apply_province_mood(World& world, ProvinceId province, const ProvinceMood& result);

/// mood_input + compute_province_mood + apply_province_mood: step 5 (first half) of the slot of a
/// province. Returns what it computed (all zero for a world without economy or an unknown province).
ProvinceMood step_province_mood(World& world, const EconomyRules& rules, ProvinceId province);

// ---- explanations (the same code path as the simulation) ------------------------------------------

/// The breakdown of the target mood of a group of a province, by the same code as the month.
[[nodiscard]] Breakdown explain_mood(const EconomyRules& rules, const MoodInput& input, Group group);

}  // namespace pat::sim::economy
