#pragma once

#include "pat/core/fixed.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/economy/types.hpp"
#include "pat/sim/entity_ids.hpp"

#include <array>
#include <cstdint>

// Unrest of a province and one-off riots (E9-T11, docs/dev/economy-model.md section 9 formulas F9 and
// F10 and section 7.3 steps 5 and 7, GDD 03 section 4.3).
//
// Everything below the "province" line is a pure function of its arguments: integer-only, no World.
// The only random numbers of the economy are drawn in `step_province_riot` (and `roll_riot`), from the
// stream `economy.unrest`; every other function here is deterministic from its arguments. The World is
// touched in `unrest_input`, `step_province_unrest` and `step_province_riot`; money moves ONLY through
// World::post_money.
//
// F9 - the unrest of a province (points, 0..100) tends to a target and moves toward it a part of the gap
// a month (`UnrestRules::inertia_permille`, `approach` of administrator.hpp: truncated toward zero, at
// least one raw unit until the target is reached):
//
//   u_g      = clamp(0..100, -mood_g x slope)                 only a sullen group adds unrest
//   w_g      = size_g x influence[g]                          who counts, and how much
//   target   = sum(w_g x u_g) / sum(w_g)                      the average over the people
//            + control_slope x max(0, low_threshold - control)  a weak grip breeds disorder
//            + events                                         hook of E11 (nothing writes it yet)
//            clamped to 0..100
//
// A province without people has no average (0); its unrest comes from control and events only. The
// contributions of the groups are split by cumulative truncation, so they add up to the average
// exactly (Breakdown). The mood read is the one step 5 has just written (`mood.hpp`).
//
// The state of a province is a pure function of its unrest, lower limit INCLUSIVE and upper EXCLUSIVE:
//   unrest <  calm_below  (25)              Calm
//   unrest <  riot_from   (50)              Agitation   (taxes and recruits fall, theft grows)
//   unrest <  uprising_from (75)             Riot        (one-off riots are possible)
//   otherwise                                Uprising    (the "flag" of the GDD: a rebel militia, M2)
// Unrest is a Fixed32, so 24.99 is still Calm and 49.99 is still Agitation. Nothing is stored for the
// state: it is derived from the column, and the previous month's state is derived from the value the
// column had before the step. A change of state is written to the incident log (IncidentKind
// UnrestStateChanged, `param` = the new state), and entering the uprising writes IncidentKind Uprising
// too (`param` = the unrest in whole points, rounded down), once per entry.
//
// F10 - one-off riot. When the unrest of a province, after step 5, is at least `riot_from`:
//   chance = lerp(riot_chance_min, riot_chance_max)  per-mille, linear from `riot_from` to 100
//            (truncated toward zero);
//   one draw `bounded(1000) < chance` from the stream `economy.unrest` - exactly ONE draw per province
//            per slot whenever the unrest is at least `riot_from`, whatever the chance and the outcome
//            (so the number of draws never depends on the result of one);
//   on success: loot = income_last_month x riot_loot_permille, posted as LedgerCode::RaidLoss against the
//            account that gets the income of the province; control falls by `riot_control_loss` points
//            (not below 0); IncidentKind Riot is written (`param` = the loot in kopecks, saturating at
//            INT32_MAX). `income_last_month` itself is not reduced (it is the base of the next riot,
//            not an account). The riot happens in an ownerless province too (no loot, no entry for a
//            dynasty); the number of draws is the same.

namespace pat::core {
class Pcg32;
}

namespace pat::sim {
class World;
}

namespace pat::sim::economy {

// ---- state -------------------------------------------------------------------------------------

enum class UnrestState : std::uint8_t {
    Calm = 0,
    Agitation = 1,
    Riot = 2,
    Uprising = 3,
};
inline constexpr std::uint8_t kUnrestStateCount = 4;

/// The state of an unrest value (lower limits inclusive, see the top of the file).
[[nodiscard]] UnrestState unrest_state(const UnrestRules& rules, core::Fixed32 unrest) noexcept;

/// True from `uprising_from` upward: the "uprising" flag of the GDD.
[[nodiscard]] bool is_uprising(const UnrestRules& rules, core::Fixed32 unrest) noexcept;

// ---- F9 ------------------------------------------------------------------------------------------

/// Everything F9 reads about one province, as plain values.
struct UnrestInput {
    std::array<std::uint32_t, kGroupCount> size{};
    std::array<core::Fixed32, kGroupCount> mood{};
    core::Fixed32 control = kDefaultControl;  ///< 0..100
    core::Fixed32 unrest;                     ///< 0..100, before the month
    /// Hook of E11: unrest added by events, points; nothing writes it yet.
    core::Fixed32 events;
};

/// The month of a province.
struct UnrestResult {
    core::Fixed32 target;
    core::Fixed32 unrest;  ///< after the month
    UnrestState before = UnrestState::Calm;
    UnrestState after = UnrestState::Calm;
    [[nodiscard]] bool operator==(const UnrestResult&) const = default;
};

/// The unrest a group adds while it is sullen: clamp(0..100, -mood x slope), points.
[[nodiscard]] core::Fixed32 group_unrest(const UnrestRules& rules, core::Fixed32 mood) noexcept;

/// The unrest the province tends to. Root of the breakdown: term::kUnrestTarget (Points); children:
/// kUnrestGroupFirst + g for every group that contributes (a contribution that is 0 is left out),
/// kUnrestControl, kUnrestEvents (left out when 0) and kUnrestLimit (the step the clamp to 0..100 made).
/// Sizes above kMaxProvincePersons count as that many people.
[[nodiscard]] core::Fixed32
compute_unrest_target(const EconomyRules& rules, const UnrestInput& input, Breakdown* breakdown = nullptr);

/// F9: the target and one month of movement. The unrest stays within 0..100 whatever the input (a
/// column written out of its range is pulled back into it); `before` classifies the value it had.
[[nodiscard]] UnrestResult compute_unrest(const EconomyRules& rules, const UnrestInput& input);

// ---- F10 -----------------------------------------------------------------------------------------

/// Monthly chance of a riot in per-mille: 0 below `riot_from`, `riot_chance_min` at it, rising linearly
/// to `riot_chance_max` at 100 unrest.
[[nodiscard]] std::int32_t riot_chance_permille(const UnrestRules& rules, core::Fixed32 unrest) noexcept;

/// The one draw of a province: `bounded(1000) < chance_permille`; always consumes exactly one number
/// of the generator, even for a chance of 0 or 1000.
[[nodiscard]] bool roll_riot(core::Pcg32& rng, std::int32_t chance_permille);

/// What a riot takes of the income of the month (truncated toward zero; 0 for a negative income).
[[nodiscard]] Kopecks riot_loot(const UnrestRules& rules, Kopecks income_last_month) noexcept;

/// The control left after a riot: `riot_control_loss` points less, not below 0.
[[nodiscard]] core::Fixed32 control_after_riot(const UnrestRules& rules, core::Fixed32 control) noexcept;

/// What step 7 did to a province.
struct RiotOutcome {
    bool rolled = false;    ///< the unrest was at least `riot_from`: one number was drawn
    bool happened = false;  ///< the draw was a success: there was a riot
    std::int32_t chance_permille = 0;
    Kopecks loot_kop = 0;     ///< what the owner's account actually lost (RaidLoss)
    Kopecks refused_kop = 0;  ///< what the account did not take of the loot (saturation), see section 3
    core::Fixed32 control;    ///< control after the step (unchanged when no riot happened)
    [[nodiscard]] bool operator==(const RiotOutcome&) const = default;
};

// ---- province ------------------------------------------------------------------------------------

/// The input of a province as the World holds it; a default input for a world without economy or an
/// unknown province.
[[nodiscard]] UnrestInput unrest_input(const World& world, ProvinceId province);

/// unrest_input + compute_unrest + the write of the column + the incident log (state changes and the
/// start of an uprising): step 5 (second half) of the slot of a province. Returns what it computed
/// (all zero for a world without economy or an unknown province).
UnrestResult step_province_unrest(World& world, const EconomyRules& rules, ProvinceId province);

/// Step 7 of the slot of a province: the draw, the loot, the fall of control, the entry "Riot".
RiotOutcome step_province_riot(World& world, const EconomyRules& rules, ProvinceId province);

// ---- explanations (the same code path as the simulation) ------------------------------------------

/// The breakdown of the target unrest of a province, by the same code as the month.
[[nodiscard]] Breakdown explain_unrest(const EconomyRules& rules, const UnrestInput& input);

}  // namespace pat::sim::economy
