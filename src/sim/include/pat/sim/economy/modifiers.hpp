#pragma once

#include "pat/sim/economy/administrator.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/types.hpp"
#include "pat/sim/entity_ids.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Modifiers of a province and their cache (E9-T17, docs/dev/economy-model.md sections 4.10, 9 F16 and
// 17.10, GDD 03 sections 8.2 and 13).
//
// A building is "a set of modifiers, not special logic" (GDD 03 section 8.2). All that the economy needs to
// know about a province besides its columns is gathered in ONE plain value, `ProvinceModifiers`:
//
//   from the READY buildings (the slot grid; a construction under way has no effect, like no upkeep):
//     output_permille[g]       multiplier of the output of good g (F3): 1000 + the sum over buildings of
//                              (their multiplier - 1000), clamped to 0..kMaxMultiplierPermille
//     control_points           added to the target of control (F7): the sum of control_points
//     oversight_permille       taken off the target of corruption (F7): the sum, clamped to 0..1000
//     recruit_permille         multiplier of the recruit potential (F11): 1000 + the sum of the deltas
//     famine_protect_permille  the share of a grain shortage the people do not feel (F5): the sum, 0..1000
//   from the POLICY of the owner (the level the province is processed at, policy.hpp):
//     tax_level, recruit_level (F6, F8, F11)
//   from the ADMINISTRATOR of the province (administrator.hpp):
//     stewardship, integrity (F7); the neutral profile when nobody governs
//
// The effects of several buildings ADD (as deltas around 1000, the usual way of modifier sets) - the
// result does not depend on the order of the slots - and the sum is clamped to the range of its unit.
//
// DERIVED DATA. The set of a province is a pure function of the world and the rules: nothing here is state.
// `compute_province_modifiers` is that function (the "direct computation"). The cache `ModifierCache`
// only remembers its results so a monthly pass does not repeat them; it lives inside the World (so that
// the mutators of the World can mark it) but is NOT in the save, the hash or any comparison, and a
// loaded world starts with everything dirty. The consumers (the steps of the slot, the forecast, the
// explanations) ask `province_modifiers`, which answers from the cache when the entry is clean and by
// direct computation otherwise - so the cache can never change a result, only the cost of it.
//
// DIRTY FLAGS are set by the World itself, where the inputs change:
//   a building appears or goes (set_slot_building, hence also finish_due_constructions),
//   the owner changes (set_province_owner, set_province_owner_kind) - the policy of the new owner and a
//   dropped administrator follow,
//   the administrator changes (set_province_administrator, a dead or destroyed administrator is removed,
//   a foreign one is released, a skill or a trait of an administrator changes),
//   the tax or the recruit policy of a dynasty changes (set_dynasty_tax_policy, set_dynasty_recruit_policy
//   mark every province of the dynasty),
//   a province is added.
// What the World cannot see: the columns `tax_policy` / `recruit_policy` of a PROVINCE written through
// economy_columns_mut() (scenarios; no command writes them) - such a writer calls
// World::invalidate_province_modifiers; and a change of the RULES, which the system `economy.provinces`
// notices by the stamp of the rules (`modifier_rules_stamp`) and answers with a full invalidation. The
// invariant E-R3 (invariants.hpp) compares every clean entry with the direct computation, so a missed
// flag is a failing check, not a silent error.
//
// Budget: a full rebuild of 400 provinces <= 200 ms (GDD 03 section 13 [predl.]); measured in
// docs/benchmarks/E9-T17-building-effects.md (a few hundred microseconds).

namespace pat::sim {
class World;
}

namespace pat::sim::economy {

/// The effects of the state of a province on the formulas of the economy. Neutral by default (no
/// building, no policy chosen, nobody governing).
struct ProvinceModifiers {
    std::array<std::int16_t, kMaxGoods> output_permille = [] {
        std::array<std::int16_t, kMaxGoods> ones{};
        ones.fill(static_cast<std::int16_t>(kPermilleOne));
        return ones;
    }();
    std::int16_t control_points = 0;
    std::int16_t oversight_permille = 0;
    std::int16_t recruit_permille = static_cast<std::int16_t>(kPermilleOne);
    std::int16_t famine_protect_permille = 0;
    std::uint8_t tax_level = 0;
    std::uint8_t recruit_level = 0;
    std::uint8_t stewardship = 50;  ///< AdministratorProfile::stewardship, 0..100
    std::uint8_t integrity = 50;    ///< AdministratorProfile::integrity, 0..100

    [[nodiscard]] AdministratorProfile profile() const noexcept {
        return AdministratorProfile{.stewardship = stewardship, .integrity = integrity};
    }
    /// The same set without the administrator: what the buildings and the policy say, comparable across
    /// administrator sources (used by the invariant).
    [[nodiscard]] bool same_without_profile(const ProvinceModifiers& other) const noexcept {
        ProvinceModifiers a = *this;
        a.stewardship = other.stewardship;
        a.integrity = other.integrity;
        return a == other;
    }

    [[nodiscard]] bool operator==(const ProvinceModifiers&) const = default;
};

/// What the cache remembers: one set and one clean/dirty bit per province, plus the stamp of the rules the
/// entries were made with. Plain data; the functions below fill it. Lazily sized: an index past the end is
/// dirty, so a freshly loaded or copied world needs no set-up.
class ModifierCache {
public:
    /// True when the entry of `province` is valid.
    [[nodiscard]] bool clean(std::size_t province) const noexcept {
        return province < clean_.size() && clean_[province] != 0;
    }
    /// The entry of a clean province (unspecified for a dirty one: check `clean`).
    [[nodiscard]] const ProvinceModifiers& value(std::size_t province) const noexcept {
        return values_[province];
    }
    /// Number of clean entries; with `province_count` it tells whether anything is dirty.
    [[nodiscard]] std::size_t clean_count() const noexcept { return clean_count_; }

    /// Remembers the set of a province and marks it clean.
    void store(std::size_t province, const ProvinceModifiers& modifiers);
    /// Marks one province / all provinces dirty.
    void mark(std::size_t province) noexcept;
    void mark_all() noexcept;

    /// The stamp of the rules the clean entries belong to (0 = none yet).
    [[nodiscard]] std::uint64_t stamp() const noexcept { return stamp_; }
    void set_stamp(std::uint64_t stamp) noexcept { stamp_ = stamp; }

private:
    std::vector<ProvinceModifiers> values_;
    std::vector<std::uint8_t> clean_;
    std::size_t clean_count_ = 0;
    std::uint64_t stamp_ = 0;
};

// ---- the direct computation ----------------------------------------------------------------------

/// A hash of everything in `rules` that the modifiers read (the building types and the default policy
/// levels); never 0. Equal rules give equal stamps.
[[nodiscard]] std::uint64_t modifier_rules_stamp(const EconomyRules& rules) noexcept;

/// The set of a province computed from the world and the rules, from scratch. `administrators` gives the
/// profile of the administrator (null = the stub, as everywhere). A neutral set for a world without
/// economy or an unknown province. Pure; the one definition of every number in the cache.
[[nodiscard]] ProvinceModifiers compute_province_modifiers(const World& world,
                                                           const EconomyRules& rules,
                                                           const AdministratorSource* administrators,
                                                           ProvinceId province);

/// The set of a province as the consumers read it: the cached entry when it is clean, otherwise the direct
/// computation (nothing is stored: a const world cannot be refreshed). Always equal to
/// compute_province_modifiers when no input changed without a flag.
[[nodiscard]] ProvinceModifiers province_modifiers(const World& world,
                                                   const EconomyRules& rules,
                                                   const AdministratorSource* administrators,
                                                   ProvinceId province);

/// Recomputes and stores the entry of a province unless it is clean already. True when it computed.
bool refresh_province_modifiers(World& world,
                                const EconomyRules& rules,
                                const AdministratorSource* administrators,
                                ProvinceId province);

/// Recomputes every dirty entry; returns how many. Costs one comparison when everything is clean.
/// This is what the system `economy.provinces` runs daily; after a load it is the full rebuild.
std::size_t
refresh_modifier_cache(World& world, const EconomyRules& rules, const AdministratorSource* administrators);

/// Marks everything dirty and recomputes it (and sets the stamp of the rules); returns the number of
/// provinces. The measured operation of the budget.
std::size_t
rebuild_modifier_cache(World& world, const EconomyRules& rules, const AdministratorSource* administrators);

// ---- bounds and explanations ---------------------------------------------------------------------

/// The largest recruit multiplier (per-mille) that any legal set of buildings can give (at most
/// `slots` buildings, at most `max_per_province` of a type): the bound of the invariant E-R1 on the
/// recruit pool. At least 1000.
[[nodiscard]] std::int32_t max_building_recruit_permille(const EconomyRules& rules) noexcept;

/// Which effect of the buildings an explanation is about.
enum class ModifierKind : std::uint8_t {
    Output,            ///< output_permille of one good
    Control,           ///< control_points
    Oversight,         ///< oversight_permille
    Recruits,          ///< recruit_permille
    FamineProtection,  ///< famine_protect_permille
};

/// The breakdown of one building effect of a province: the root says the total (the number that is in
/// the modifier set), its children are the contributions of the ready buildings, each with the code
/// term::building_term(slot) and its delta (`factor_permille` = the multiplier for the multiplicative
/// effects), a base term of 1000 for the multipliers, and a limit term when the sum was clamped. Roots:
/// kOutputBuildingsTotal, kControlBuildingsTotal, kCorruptionOversightTotal, kRecruitBuildingsTotal,
/// kWealthFamineTotal. `good` is read for Output only. The total equals the field of
/// compute_province_modifiers (control in Points = whole points as Fixed32 raw). Empty for a world without
/// economy, an unknown province, or a good out of range.
[[nodiscard]] Breakdown explain_modifier(const World& world,
                                         const EconomyRules& rules,
                                         ProvinceId province,
                                         ModifierKind kind,
                                         std::size_t good = 0);

}  // namespace pat::sim::economy
