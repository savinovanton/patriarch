#pragma once

#include "pat/core/fixed.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/economy/types.hpp"
#include "pat/sim/entity_ids.hpp"

#include <cstdint>

// Administrator, control and corruption of a province (E9-T10, docs/dev/economy-model.md section 9
// formula F7 and section 7.3 step 1, GDD 03 sections 3.2 and 7.4).
//
// Everything below the "province" line is a pure function of its arguments: integer-only, no random
// numbers, no World. The World is touched in exactly two places - `administration_input` reads one
// province into a plain struct and `apply_province_administration` writes the two columns back - so
// the formula can be tested, explained (Breakdown) and replaced without a world.
//
// Control (points, 0..100) and corruption (a share of the tax, 0..1) both move toward a target a part
// of the gap per month (the inertia of the rules, truncated toward zero so a column never jumps over
// its target):
//
//   control_target    = control_base
//                     + steward_gain x (stewardship - neutral)             the administrator
//                     + building_control                                   hook of E9-T17 (0 so far)
//                     - unrest_penalty x unrest                            disorder at home
//                     clamped to 0..100
//   corruption_target = corruption_base
//                     - integrity_gain x (integrity - neutral)             the administrator
//                     - oversight                                          hook of E9-T17 (0 so far)
//                     + low_control_corruption x (low_threshold - control) only while control is low
//                     clamped to 0..1 (0..1000 per-mille)
//
// The "tenure bonus" of the GDD (the longer a house holds a province the better it governs it) needs
// the date the province changed hands, which no table keeps; it stays out until a task adds that column.
//
// The thresholds of the GDD (40 and 15) are classified in ONE place, `control_state`, and every reader of
// control - the taxes (F6), the unrest (F9), the recruits (F11) - asks it:
//   control <  anarchy_threshold (15)       Anarchy: the province is in turmoil, the owner gets nothing
//   control <  low_threshold (40)           Low: less is collected, corruption grows, risk of riot
//   otherwise                               Normal
// Both comparisons are STRICT: control exactly 40 is Normal and control exactly 15 is Low (not Anarchy).
// Control is a Fixed32, so 39.99 is already Low and 14.99 already Anarchy.
//
// An administrator is a character (Dynasty of the owner or, for a system actor, anyone). Who is a good
// one comes from `administrator_profile` (E7 will map the skill Stewardship 0..20 x5 and the traits
// into 0..100); until E7 it is a stub that gives every administrator 50/50, i.e. the neutral
// profile, and tests plug their own `AdministratorSource`. A province without an administrator is
// governed by the owner with the neutral profile (no bonus, no penalty): the difference between "nobody"
// and "an average one" is for E10 (offices) to introduce.

namespace pat::sim {
class World;
}

namespace pat::sim::economy {

/// Skills of an administrator as the economy sees them, 0..100 (50 = neutral, no effect).
struct AdministratorProfile {
    std::int32_t stewardship = 50;  ///< raises control
    std::int32_t integrity = 50;    ///< lowers corruption

    [[nodiscard]] bool operator==(const AdministratorProfile&) const = default;
};
inline constexpr std::int32_t kProfileMax = 100;

/// Where the profile of an administrator comes from (E7 replaces the implementation; tests plug in
/// their own). The result must be a deterministic function of the world and the character; the caller
/// clamps both numbers to 0..100.
class AdministratorSource {
public:
    virtual ~AdministratorSource() = default;
    [[nodiscard]] virtual AdministratorProfile profile(const World& world, CharacterId character) const = 0;
};

/// The stub until E7 lands: `administrator_profile`, 50/50 for everybody.
class StubAdministratorSource final : public AdministratorSource {
public:
    [[nodiscard]] AdministratorProfile profile(const World& world, CharacterId character) const override;
};

/// `{stewardship, integrity}` of a character, 0..100 each (docs/dev/economy-model.md section 15.1).
/// Until E7 it is the neutral stub {50, 50}.
[[nodiscard]] AdministratorProfile administrator_profile(const World& world, CharacterId character);

// ---- control state -------------------------------------------------------------------------------

enum class ControlState : std::uint8_t {
    Normal = 0,
    Low = 1,      ///< control < low_threshold: collection falls, corruption grows
    Anarchy = 2,  ///< control < anarchy_threshold: turmoil, the owner gets nothing
};

/// The state of a control value (both limits strict, see the top of the file).
[[nodiscard]] ControlState control_state(const ControlRules& rules, core::Fixed32 control) noexcept;

// ---- the formula ---------------------------------------------------------------------------------

/// Everything F7 reads about one province, as plain values.
struct AdministrationInput {
    core::Fixed32 control = kDefaultControl;  ///< 0..100
    core::Fixed32 corruption;                 ///< 0..1
    core::Fixed32 unrest;                     ///< 0..100
    /// The profile of the administrator; the neutral one when there is nobody (see the top of the file).
    AdministratorProfile profile;
    /// Hooks of E9-T17 (the modifiers of buildings): control points added and corruption per-mille
    /// removed. Zero until buildings have effects.
    core::Fixed32 building_control;
    std::int32_t oversight_permille = 0;
};

/// The two values of the next month.
struct Administration {
    core::Fixed32 control_target;
    core::Fixed32 corruption_target;
    core::Fixed32 control;     ///< control after the month
    core::Fixed32 corruption;  ///< corruption after the month

    [[nodiscard]] bool operator==(const Administration&) const = default;
};

/// The control the province tends to. Root of the breakdown: term::kControlTarget (Points); children:
/// kControlBase, kControlAdministrator, kControlBuildings, kControlUnrest (a term that is 0 is left out
/// except the base) and kControlLimit (the step that the clamp to 0..100 made, if any).
[[nodiscard]] core::Fixed32 compute_control_target(const ControlRules& rules,
                                                   const AdministrationInput& input,
                                                   Breakdown* breakdown = nullptr);

/// The corruption the province tends to. Root: term::kCorruptionTarget (Permille); children:
/// kCorruptionBase, kCorruptionIntegrity (negative above neutral), kCorruptionOversight (negative),
/// kCorruptionLowControl and kCorruptionLimit (the clamp to 0..1000). The returned value is the
/// smallest Fixed32 that reads back as the target in per-mille (so a column that reached its target
/// reads exactly that many per-mille).
[[nodiscard]] core::Fixed32 compute_corruption_target(const ControlRules& rules,
                                                      const AdministrationInput& input,
                                                      Breakdown* breakdown = nullptr);

/// Moves `current` toward `target` by `inertia_permille` of the gap, truncated toward zero.
/// Positive inertia moves at least one raw unit until the target is reached, without crossing it.
/// Zero inertia freezes the value; inertia is clamped to 0..1000.
[[nodiscard]] core::Fixed32
approach(core::Fixed32 current, core::Fixed32 target, std::int32_t inertia_permille) noexcept;

/// F7: both targets and one month of movement. Control stays within 0..100, corruption within 0..1.
[[nodiscard]] Administration compute_administration(const ControlRules& rules,
                                                    const AdministrationInput& input);

// ---- province ------------------------------------------------------------------------------------

/// The input of a province as the World holds it (the profile comes from `source`, which may be
/// null for the stub); a default input for a world without economy or an unknown province.
[[nodiscard]] AdministrationInput
administration_input(const World& world, const AdministratorSource* source, ProvinceId province);

/// Writes control and corruption of a result into the columns of the province.
void apply_province_administration(World& world, ProvinceId province, const Administration& result);

/// compute_administration + apply_province_administration: step 1 of the slot of a province
/// (section 7.3). Returns what it computed.
Administration step_province_administration(World& world,
                                            const EconomyRules& rules,
                                            const AdministratorSource* source,
                                            ProvinceId province);

/// The entry of the incident log for a province whose income is withheld by turmoil (kind Anarchy,
/// `param` = the control in whole points, rounded down): written once per month the province is in
/// turmoil, on its slot day, and only when somebody would have been paid (a province held by a dynasty
/// or the crown). Returns true when an entry was written. `income.anarchy` comes from step 4 (F6).
bool record_anarchy(World& world, ProvinceId province, bool anarchy);

// ---- explanations (the same code path as the simulation) ------------------------------------------

[[nodiscard]] Breakdown explain_control(const EconomyRules& rules, const AdministrationInput& input);
[[nodiscard]] Breakdown explain_corruption(const EconomyRules& rules, const AdministrationInput& input);

}  // namespace pat::sim::economy
