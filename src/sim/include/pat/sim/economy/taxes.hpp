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
#include <optional>

// Taxes and income of a province (E9-T08, docs/dev/economy-model.md section 9 formula F6 and
// section 7.3 step 4, GDD 03 sections 3.2, 3.2a and 7.1).
//
// Everything below the "province" line is a pure function of its arguments: integer-only, no random
// numbers, no World. The World is touched in exactly two places - `tax_input` reads one province into
// a plain struct and `post_province_income` writes the money - so the formula can be tested, explained
// (Breakdown) and replaced without a world. Money moves ONLY through World::post_money.
//
// The tax of a group (F6), kopecks per month:
//   base   = size x head_tax_kop_year[group] / 12                       (truncated)
//   gross  = base x policy.tax x wealth/100 x control/100 x law [x low_control_tax]
//            (per-mille multipliers applied in this order and truncated toward zero; the last one only
//            while control is below `control.low_threshold`)
//   gross  = 0 while control is below `control.anarchy_threshold`: the province is in turmoil and
//            nothing is collected (the interface with E9-T10, which records the incident)
//   leak   = gross x corruption                                         (the collector's theft)
//   crown  = (gross - leak) x city_crown_share, only for the townsfolk of a City held by a dynasty
//   owner  = gross - leak - crown                                       (so the parts add up exactly)
// "Efficiency" of the GDD is the product of the control, the corruption and the law factors: they
// are the three terms above that are not rates or policy.
//
// Postings, on the day of the slot of the province (step 4 of the order of a slot):
//   owner account   +(gross - crown) on tax_<group>   -leak on embezzlement   +holding income
//   crown account   +crown on tax_townsfolk           (only when the crown is not the owner)
// so the postings of a month add up to the sum of the net taxes (leak and crown share included) and
// a kopeck is neither lost nor made by the split. Who the owner is comes from the owner kind: a dynasty
// is paid to its account, the crown to the crown account, nobody (Unowned, Church, Free: not used in
// M1) gets nothing, and then the taxes are computed but not posted.

namespace pat::sim {
class World;
}

namespace pat::sim::economy {

/// Everything F6 reads about one province, as plain values.
struct TaxInput {
    std::array<std::uint32_t, kGroupCount> size{};
    std::array<core::Fixed32, kGroupCount> wealth{};
    core::Fixed32 control = kDefaultControl;
    core::Fixed32 corruption;  ///< 0..1
    ProvinceKind kind = ProvinceKind::Rural;
    OwnerKind owner_kind = OwnerKind::Unowned;
    std::uint8_t tax_level = 0;  ///< index into `TaxRules::levels`; an index past the table is clamped
};

/// The tax of one group, split by subtraction: gross == leak + crown + owner.
struct GroupTax {
    Kopecks gross = 0;  ///< what the group pays this month
    Kopecks leak = 0;   ///< stolen on the way (embezzlement)
    Kopecks crown = 0;  ///< the crown's share of the taxes of a city held by a dynasty
    [[nodiscard]] Kopecks owner() const noexcept { return gross - leak - crown; }
    [[nodiscard]] bool operator==(const GroupTax&) const = default;
};

/// What the owner and the crown get of a province in one month.
struct ProvinceIncome {
    std::array<GroupTax, kGroupCount> group{};
    /// The holding income the owner gets (the input of the step, 0 in turmoil).
    Kopecks holding = 0;
    /// True while control is below the anarchy threshold: nothing is collected, nothing is paid.
    bool anarchy = false;

    [[nodiscard]] Kopecks gross_total() const noexcept;
    [[nodiscard]] Kopecks leak_total() const noexcept;
    [[nodiscard]] Kopecks crown_total() const noexcept;
    /// Net of the taxes: what stays after the leak, the crown's share included.
    [[nodiscard]] Kopecks net_total() const noexcept { return gross_total() - leak_total(); }
    /// The taxes the owner keeps plus the holding income.
    [[nodiscard]] Kopecks owner_total() const noexcept;

    [[nodiscard]] bool operator==(const ProvinceIncome&) const = default;
};

// ---- the formula ---------------------------------------------------------------------------------

/// A multiplier in per-mille of a point value (wealth, ...): raw x 10 / 65536 so 100 points is 1000,
/// truncated, clamped to 0..3000.
[[nodiscard]] std::int32_t points_permille(core::Fixed32 points) noexcept;
/// Corruption (0..1) as per-mille, truncated, clamped to 0..1000.
[[nodiscard]] std::int32_t corruption_permille(core::Fixed32 corruption) noexcept;

/// The monthly head tax before any multiplier: size x rate_per_year / 12, truncated, capped at
/// kMaxPostingKop.
[[nodiscard]] Kopecks tax_base(std::uint32_t size, Kopecks head_tax_kop_year) noexcept;

/// F6: the tax of one group. Root of the breakdown: term::kTaxTotal (Kopecks) = what the owner keeps
/// (`owner()`); children: kTaxBase, then one step per multiplier that is not 1000 (kTaxPolicy,
/// kTaxWealth, kTaxControl, kTaxLaw, kTaxLowControl; the multiplier is in `factor_permille`),
/// kTaxAnarchy (a step to zero), kTaxLeak (negative) and kTaxCrownShare (negative); a term that is 0
/// is left out except the base. An unknown group gives a zero tax.
[[nodiscard]] GroupTax compute_group_tax(const TaxRules& tax,
                                         const ControlRules& control,
                                         const TaxInput& input,
                                         Group group,
                                         Breakdown* breakdown = nullptr);

/// F6 for the six groups plus the holding income of the month (the owner gets nothing in turmoil).
[[nodiscard]] ProvinceIncome
compute_province_income(const EconomyRules& rules, const TaxInput& input, Kopecks holding_income_kop);

// ---- province ------------------------------------------------------------------------------------

/// The input of a province as the World holds it; a default input for a world without economy or an
/// unknown province.
[[nodiscard]] TaxInput tax_input(const World& world, ProvinceId province);

/// The account that receives what a province earns: the account of the owning dynasty, the crown
/// account, or nobody (Unowned, Church, Free; a dynasty owner that does not exist).
[[nodiscard]] std::optional<AccountRef> income_recipient(const World& world, ProvinceId province);

/// Posts `income` to the accounts through World::post_money (the owner's taxes, the leak and the
/// holding income; the crown's share to the crown) and writes the net income of the owner into the
/// column `income_last_month` (0 without a recipient). Returns that income. Posting order: groups in
/// ascending order (tax, then leak), the crown's share, the holding income.
Kopecks post_province_income(World& world, ProvinceId province, const ProvinceIncome& income);

/// tax_input + compute_province_income + post_province_income: what the system `economy.provinces`
/// runs on the slot day of a province (step 4 of section 7.3). `holding_income_kop` is the result of
/// step 3 (E9-T07). Returns the income that was computed (before saturation of an account).
ProvinceIncome
step_province_taxes(World& world, const EconomyRules& rules, ProvinceId province, Kopecks holding_income_kop);

// ---- explanations (the same code path as the simulation) ------------------------------------------

/// The breakdown of the tax of a group of a province, by the same code as the month.
[[nodiscard]] Breakdown explain_group_tax(const EconomyRules& rules, const TaxInput& input, Group group);

}  // namespace pat::sim::economy
