#include "pat/sim/economy/taxes.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/economy/administrator.hpp"
#include "pat/sim/economy/ledger.hpp"
#include "pat/sim/economy/modifiers.hpp"
#include "pat/sim/economy/policy.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <cstdlib>

namespace pat::sim::economy {

using core::Fixed32;

namespace {

constexpr std::int64_t kOne = Fixed32::one_raw;
constexpr std::int64_t kMonths = 12;
/// Wealth is 0..300 points, so its multiplier is at most 3000 per-mille.
constexpr std::int32_t kMaxWealthPermille = 3000;

/// The multiplier of the policy level of a province; a level past the table is the last one.
std::int32_t policy_permille(const TaxRules& tax, std::uint8_t level) noexcept {
    if (tax.level_count <= 0) {
        return kPermilleOne;
    }
    const auto last = static_cast<std::size_t>(tax.level_count - 1);
    return tax.levels[std::min<std::size_t>(level, last)].tax_permille;
}

/// One step of the chain: x per-mille, truncated toward zero, never above a posting.
Kopecks step_permille(Kopecks running, std::int32_t permille) noexcept {
    return std::min(mul_permille(running, std::max(0, permille)), kMaxPostingKop);
}

}  // namespace

// ---- ProvinceIncome ------------------------------------------------------------------------------

Kopecks ProvinceIncome::gross_total() const noexcept {
    Kopecks sum = 0;
    for (const GroupTax& g : group) {
        sum += g.gross;
    }
    return sum;
}

Kopecks ProvinceIncome::leak_total() const noexcept {
    Kopecks sum = 0;
    for (const GroupTax& g : group) {
        sum += g.leak;
    }
    return sum;
}

Kopecks ProvinceIncome::crown_total() const noexcept {
    Kopecks sum = 0;
    for (const GroupTax& g : group) {
        sum += g.crown;
    }
    return sum;
}

Kopecks ProvinceIncome::owner_total() const noexcept {
    Kopecks sum = holding;
    for (const GroupTax& g : group) {
        sum += g.owner();
    }
    return sum;
}

// ---- F6 -------------------------------------------------------------------------------------------

std::int32_t points_permille(Fixed32 points) noexcept {
    const std::int64_t scaled = static_cast<std::int64_t>(points.raw()) * 10 / kOne;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(scaled, 0, kMaxWealthPermille));
}

std::int32_t corruption_permille(Fixed32 corruption) noexcept {
    const std::int64_t scaled = static_cast<std::int64_t>(corruption.raw()) * kPermilleOne / kOne;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(scaled, 0, kPermilleOne));
}

Kopecks tax_base(std::uint32_t size, Kopecks head_tax_kop_year) noexcept {
    if (size == 0 || head_tax_kop_year <= 0) {
        return 0;
    }
    // size < 2^32 and the rate is at most a posting: the product needs 128 bits
    const auto base = checked_mul_div(static_cast<Kopecks>(size), head_tax_kop_year, kMonths);
    return base ? std::min(*base, kMaxPostingKop) : kMaxPostingKop;
}

GroupTax compute_group_tax(const TaxRules& tax,
                           const ControlRules& control_rules,
                           const TaxInput& input,
                           Group group,
                           Breakdown* breakdown) {
    const std::size_t g = index_of(group);
    if (g >= kGroupCount) {
        if (breakdown != nullptr) {
            breakdown->begin(term::kTaxTotal, Unit::Kopecks, 0);
        }
        return {};
    }

    // the thresholds are classified in one place (administrator.hpp): both limits are strict
    const ControlState state = control_state(control_rules, input.control);
    const bool anarchy = state == ControlState::Anarchy;
    const bool low_control = state != ControlState::Normal;

    struct Step {
        std::uint16_t code;
        std::int32_t permille;
    };
    const std::array<Step, 5> steps{{
        {term::kTaxPolicy, policy_permille(tax, input.tax_level)},
        {term::kTaxWealth, points_permille(input.wealth[g])},
        {term::kTaxControl, control_permille(input.control)},
        {term::kTaxLaw, tax.law_permille},
        {term::kTaxLowControl, low_control ? tax.low_control_tax_permille : kPermilleOne},
    }};

    const Kopecks base = tax_base(input.size[g], tax.head_tax_kop_year[g]);
    if (breakdown != nullptr) {
        breakdown->begin(term::kTaxTotal, Unit::Kopecks, 0);
        (void)breakdown->add_term(kRootNode, term::kTaxBase, base);
    }
    Kopecks running = base;
    for (const Step& step : steps) {
        const Kopecks next = step_permille(running, step.permille);
        if (breakdown != nullptr && step.permille != kPermilleOne) {
            (void)breakdown->add_step(kRootNode, step.code, running, next, step.permille);
        }
        running = next;
    }

    GroupTax out;
    if (anarchy) {
        // turmoil: nothing is collected (a step to zero in the breakdown)
        if (breakdown != nullptr && running != 0) {
            (void)breakdown->add_step(kRootNode, term::kTaxAnarchy, running, 0);
        }
        running = 0;
    }
    out.gross = running;
    const std::int32_t corruption = corruption_permille(input.corruption);
    out.leak = mul_permille(out.gross, corruption);
    if (group == Group::Townsfolk && input.kind == ProvinceKind::City &&
        input.owner_kind == OwnerKind::Dynasty) {
        // split by subtraction: the owner's part is what is left, so the three parts add up
        out.crown = mul_permille(out.gross - out.leak, tax.city_crown_share_permille);
    }
    PAT_ASSERT(out.gross >= 0 && out.leak >= 0 && out.crown >= 0 && out.gross - out.leak - out.crown >= 0,
               "the parts of a tax must be non-negative and add up");

    if (breakdown != nullptr) {
        if (out.leak != 0) {
            (void)breakdown->add_term(kRootNode, term::kTaxLeak, -out.leak, corruption);
        }
        if (out.crown != 0) {
            (void)breakdown->add_term(
                kRootNode, term::kTaxCrownShare, -out.crown, tax.city_crown_share_permille);
        }
        breakdown->set_total(out.owner());
        PAT_ASSERT(breakdown->is_consistent(), "the tax breakdown must add up to what the owner keeps");
    }
    return out;
}

ProvinceIncome
compute_province_income(const EconomyRules& rules, const TaxInput& input, Kopecks holding_income_kop) {
    ProvinceIncome out;
    out.anarchy = control_state(rules.control, input.control) == ControlState::Anarchy;
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        out.group[g] = compute_group_tax(rules.tax, rules.control, input, static_cast<Group>(g));
    }
    out.holding = out.anarchy ? 0 : std::max<Kopecks>(0, holding_income_kop);
    return out;
}

// ---- province --------------------------------------------------------------------------------------

TaxInput tax_input(const World& world, const EconomyRules& rules, ProvinceId province) {
    TaxInput in;
    if (!world.economy_present() || !world.exists(province)) {
        return in;
    }
    const EconomyState& e = world.economy();
    const std::size_t p = province.index();
    const std::size_t first = p * kGroupCount;
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        in.size[g] = e.groups.size[first + g];
        in.wealth[g] = e.groups.wealth[first + g];
    }
    in.control = e.provinces.control[p];
    in.corruption = e.provinces.corruption[p];
    in.kind = static_cast<ProvinceKind>(e.provinces.kind[p]);
    in.owner_kind = static_cast<OwnerKind>(e.provinces.owner_kind[p]);
    in.tax_level = province_modifiers(world, rules, nullptr, province).tax_level;
    return in;
}

std::optional<AccountRef> income_recipient(const World& world, ProvinceId province) {
    if (!world.economy_present() || !world.exists(province)) {
        return std::nullopt;
    }
    const std::size_t p = province.index();
    switch (static_cast<OwnerKind>(world.economy().provinces.owner_kind[p])) {
        case OwnerKind::Dynasty: {
            const DynastyId owner = world.provinces().owner[p];
            if (owner.valid() && world.resolve_account(AccountRef::of(owner))) {
                return AccountRef::of(owner);
            }
            return std::nullopt;
        }
        case OwnerKind::Crown:
            return AccountRef::crown();
        case OwnerKind::Unowned:
        case OwnerKind::Church:
        case OwnerKind::Free:
            return std::nullopt;
    }
    return std::nullopt;
}

Kopecks
post_province_income(World& world, ProvinceId province, const ProvinceIncome& income, Kopecks* refused) {
    const auto owner = income_recipient(world, province);
    Kopecks received = 0;
    Kopecks refused_total = 0;
    // One posting; what the account did not take (saturation) is counted, never silently dropped.
    const auto post = [&](AccountRef account, LedgerCode code, Kopecks amount) {
        const Kopecks applied = world.post_money(account, code, amount);
        refused_total = saturating_add(refused_total, std::abs(amount - applied));
        return applied;
    };
    if (owner) {
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            const GroupTax& tax = income.group[g];
            const LedgerCode code = tax_code(static_cast<Group>(g));
            received += post(*owner, code, tax.gross - tax.crown);
            received += post(*owner, LedgerCode::Embezzlement, -tax.leak);
            if (tax.crown > 0) {
                (void)post(AccountRef::crown(), code, tax.crown);
            }
        }
        received += post(*owner, LedgerCode::HoldingIncome, income.holding);
    }
    if (refused != nullptr) {
        *refused = refused_total;
    }
    // the income of a month is never negative (the leak is part of a tax that was paid in full)
    const Kopecks net = std::max<Kopecks>(0, received);
    world.economy_columns_mut().income_last_month[province.index()] = net;
    return net;
}

ProvinceIncome step_province_taxes(World& world,
                                   const EconomyRules& rules,
                                   ProvinceId province,
                                   Kopecks holding_income_kop) {
    if (!world.economy_present() || !world.exists(province)) {
        return {};
    }
    ProvinceIncome income =
        compute_province_income(rules, tax_input(world, rules, province), holding_income_kop);
    (void)post_province_income(world, province, income, &income.refused_kop);
    return income;
}

// ---- explanations ----------------------------------------------------------------------------------

Breakdown explain_group_tax(const EconomyRules& rules, const TaxInput& input, Group group) {
    Breakdown out;
    (void)compute_group_tax(rules.tax, rules.control, input, group, &out);
    return out;
}

}  // namespace pat::sim::economy
