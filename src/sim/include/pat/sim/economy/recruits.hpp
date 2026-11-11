#pragma once

#include "pat/core/fixed.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/types.hpp"
#include "pat/sim/entity_ids.hpp"

#include <array>
#include <cstdint>

// Recruits of a province (E9-T13, docs/dev/economy-model.md section 9 formula F11 and section 7.3 step
// 6, GDD 03 sections 9 and 10).
//
// Everything below the "province" line is a pure function of its arguments: integer-only, no random
// numbers, no World. The World is touched in `recruit_input` (reads a province into a plain struct) and
// `step_province_recruits` (writes the pool back); the people taken by a ConscriptRecruits command are
// removed by World::conscript_recruits, which uses `split_conscription` below.
//
// The potential (cap) of a province, people:
//   base   = sum over peasants, cossacks, townsfolk of size[g] x share_permille[g] / 1000
//            (each group truncated: it is a number of people)
//   policy = base x recruit_permille[level] / 1000       the recruit policy the province is processed at
//   control = policy x control_permille(control) / 1000  a province that does not obey gives few recruits
//   anarchy: control < control.anarchy_threshold -> 0    (control_state: a province in turmoil gives none)
//   quota  = minus the recruit quota still unpaid        hook of E9-T14 (`RecruitInput::quota_pending`)
//   cap    = max(0, ...)
// Once a month (step 6 of the slot) the pool moves toward the cap by `regen_permille` of the gap,
// truncated toward zero, at least one person until it arrives, and never above the cap (a cap that fell -
// fewer people, lower control, a lighter policy - cuts the pool at once). So after a slot
// `pool <= cap` holds exactly.
//
// Conscription takes people out of the pool at once and the same number out of the groups (the sizes
// shrink, so the output (F2) and the cap fall with them), and the dynasty's `manpower_reserve` grows by
// exactly that number. The mood reacts through F8 (the pressure of the recruit policy level); the
// `losses` input of population.hpp stays for epidemics and famine (M2).

namespace pat::sim {
class World;
}

namespace pat::sim::economy {

/// The groups the recruits come from (formula F11), in the order the remainder of a conscription is
/// handed out.
inline constexpr std::array<Group, 3> kRecruitGroups{Group::Townsfolk, Group::Peasants, Group::Cossacks};

/// Everything F11 reads about one province, as plain values.
struct RecruitInput {
    std::array<std::uint32_t, kGroupCount> size{};  ///< people per group
    core::Fixed32 control = core::Fixed32::from_int(100);
    std::uint8_t level = 0;  ///< index into `RecruitRules::levels`
    std::uint32_t pool = 0;  ///< the pool now
    /// Hook of E9-T14: people of the unpaid recruit quota that the potential gives up; 0 for now.
    std::uint32_t quota_pending = 0;
};

struct RecruitResult {
    std::uint32_t cap = 0;       ///< the potential this month
    std::uint32_t new_pool = 0;  ///< the pool after the month: never above `cap`
    [[nodiscard]] bool operator==(const RecruitResult&) const = default;
};

// ---- the formula ---------------------------------------------------------------------------------

/// The potential. Root: term::kRecruitCap (People) = cap; children: kRecruitBase, then one step per
/// multiplier (kRecruitPolicy, kRecruitControl, with the multiplier in `factor_permille`; a multiplier
/// of exactly 1000 is left out), kRecruitAnarchy (a step to zero) and kRecruitQuota (negative). A level
/// index past the table is clamped to the last level.
[[nodiscard]] std::uint32_t
compute_recruit_cap(const EconomyRules& rules, const RecruitInput& input, Breakdown* breakdown = nullptr);

/// One month: the cap and the pool after the regeneration. (term::kRecruitRegen is reserved for the
/// tooltip of the monthly gain; the cap's tree does not use it.)
[[nodiscard]] RecruitResult compute_recruit_pool(const EconomyRules& rules, const RecruitInput& input);

/// The highest potential the sizes can give under any policy and control, with no quota: the bound that
/// holds at every moment after a slot even when a policy or the control changed since (invariant E-R1).
[[nodiscard]] std::uint32_t recruit_ceiling(const EconomyRules& rules,
                                            const std::array<std::uint32_t, kGroupCount>& size);

/// How `count` people are taken from the recruit groups: proportionally to the sizes (integer division,
/// the remainder one person at a time to the groups in the order of kRecruitGroups, never more than a
/// group has). `count` above the people of the groups is cut to them. The result has 0 for every other
/// group.
[[nodiscard]] std::array<std::uint32_t, kGroupCount>
split_conscription(const std::array<std::uint32_t, kGroupCount>& size, std::uint32_t count) noexcept;

/// The people of the recruit groups.
[[nodiscard]] std::uint64_t recruit_population(const std::array<std::uint32_t, kGroupCount>& size) noexcept;

// ---- province ------------------------------------------------------------------------------------

/// The input of a province as the World holds it; a default input for a world without economy or an
/// unknown province. The level is the one the province is processed at (effective_recruit_level).
[[nodiscard]] RecruitInput recruit_input(const World& world, const EconomyRules& rules, ProvinceId province);

/// Writes the pool of a result into the column of the province.
void apply_province_recruits(World& world, ProvinceId province, const RecruitResult& result);

/// compute + apply: step 6 of the slot of a province (section 7.3). Returns what it computed.
RecruitResult step_province_recruits(World& world, const EconomyRules& rules, ProvinceId province);

/// The explanation of the potential of a province, as a tooltip shows it (the same code path as the
/// simulation): the Breakdown of compute_recruit_cap.
[[nodiscard]] Breakdown
explain_recruit_cap(const World& world, const EconomyRules& rules, ProvinceId province);

}  // namespace pat::sim::economy
