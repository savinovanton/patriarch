#include "pat/sim/economy/production.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/economy/administrator.hpp"
#include "pat/sim/world.hpp"

#include <algorithm>
#include <limits>

namespace pat::sim::economy {

using core::Fixed32;

namespace {

constexpr std::int64_t kOne = Fixed32::one_raw;

/// Largest ratio shown as the `factor_permille` of a node (it is an int32 and the ratio is not bounded).
constexpr std::int64_t kMaxShownRatio = 1'000'000;

std::int32_t kind_index(ProvinceKind kind) noexcept {
    const auto k = static_cast<std::size_t>(kind);
    return static_cast<std::int32_t>(k < kProvinceKindCount ? k : 0);
}

std::int32_t labour_of(const ProductionRules& rules, const ProductionInput& in, Breakdown* breakdown) {
    const std::uint64_t workers =
        static_cast<std::uint64_t>(in.size[index_of(Group::Peasants)]) + in.size[index_of(Group::Townsfolk)];
    return compute_labour_permille(rules, workers, in.labour_base, in.peasants_mood, breakdown);
}

Quantity output_of(const ProductionRules& rules,
                   const ProductionInput& in,
                   std::int32_t labour_permille,
                   std::size_t good,
                   Breakdown* breakdown) {
    return compute_output(rules,
                          in.capacity[good],
                          labour_permille,
                          in.buildings_permille[good],
                          in.control,
                          in.harvest_permille,
                          breakdown);
}

}  // namespace

// ---- prices --------------------------------------------------------------------------------------

Kopecks StaticPriceSource::price(const World&, ProvinceId, std::size_t good) const {
    return good < kMaxGoods ? prices_[good] : 0;
}

// ---- F2 labour -----------------------------------------------------------------------------------

std::int32_t control_permille(Fixed32 control) noexcept {
    const std::int64_t scaled = static_cast<std::int64_t>(control.raw()) * 10 / kOne;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(scaled, 0, kPermilleOne));
}

std::int32_t compute_labour_permille(const ProductionRules& rules,
                                     std::uint64_t workers,
                                     std::uint32_t labour_base,
                                     Fixed32 peasants_mood,
                                     Breakdown* breakdown) {
    std::int64_t ratio = kPermilleOne;
    if (labour_base > 0) {
        // workers < 2^33, so workers x 1000 < 2^43: no overflow
        ratio = static_cast<std::int64_t>(workers * static_cast<std::uint64_t>(kPermilleOne) / labour_base);
    }
    const std::int64_t clamped =
        std::clamp<std::int64_t>(ratio, rules.labour_min_permille, rules.labour_max_permille);
    // Each point of bad mood of the peasants takes `slope` per-mille of the labour (at most all of it).
    const std::int64_t gloom_raw = std::max<std::int64_t>(0, -static_cast<std::int64_t>(peasants_mood.raw()));
    const std::int64_t reduction =
        std::min<std::int64_t>(kPermilleOne, gloom_raw * rules.labour_mood_slope_permille / kOne);
    const std::int64_t labour = clamped * (kPermilleOne - reduction) / kPermilleOne;

    if (breakdown != nullptr) {
        breakdown->begin(term::kOutputLabour, Unit::Permille, labour);
        (void)breakdown->add_term(kRootNode,
                                  term::kOutputLabourWorkforce,
                                  clamped,
                                  static_cast<std::int32_t>(std::min(ratio, kMaxShownRatio)));
        if (reduction > 0) {
            (void)breakdown->add_step(kRootNode,
                                      term::kOutputLabourMood,
                                      clamped,
                                      labour,
                                      static_cast<std::int32_t>(kPermilleOne - reduction));
        }
        PAT_ASSERT(breakdown->is_consistent(), "the labour breakdown must add up to the factor");
    }
    return static_cast<std::int32_t>(labour);
}

// ---- F3 output -----------------------------------------------------------------------------------

Quantity compute_output(const ProductionRules& rules,
                        Fixed32 capacity,
                        std::int32_t labour_permille,
                        std::int32_t buildings_permille,
                        Fixed32 control,
                        std::int32_t harvest_permille,
                        Breakdown* breakdown) {
    struct Step {
        std::uint16_t code;
        std::int32_t permille;
    };
    const std::array<Step, 5> steps{{
        {term::kOutputLabour, labour_permille},
        {term::kOutputBuildings, buildings_permille},
        {term::kOutputControl, control_permille(control)},
        {term::kOutputLosses, static_cast<std::int32_t>(kPermilleOne - rules.losses_permille)},
        {term::kOutputHarvest, harvest_permille},
    }};

    Quantity running = std::max<Quantity>(0, capacity.raw());
    if (breakdown != nullptr) {
        breakdown->begin(term::kOutputTotal, Unit::Pieces, 0);
        (void)breakdown->add_term(kRootNode, term::kOutputCapacity, running);
    }
    for (const Step& step : steps) {
        const Quantity next = mul_permille(running, std::max(0, step.permille));
        if (breakdown != nullptr && step.permille != kPermilleOne) {
            (void)breakdown->add_step(kRootNode, step.code, running, next, step.permille);
        }
        running = next;
    }
    if (breakdown != nullptr) {
        breakdown->set_total(running);
        PAT_ASSERT(breakdown->is_consistent(), "the output breakdown must add up to the output");
    }
    return running;
}

// ---- F4 value and holding income -------------------------------------------------------------------

Kopecks output_value(Quantity output, Kopecks price_kop) noexcept {
    if (output <= 0 || price_kop <= 0) {
        return 0;
    }
    const auto price = static_cast<std::uint64_t>(std::min(price_kop, kMaxPostingKop));
    const auto value =
        detail::mul_div_u64(static_cast<std::uint64_t>(output), price, static_cast<std::uint64_t>(kOne));
    if (!value || *value > static_cast<std::uint64_t>(kMaxPostingKop)) {
        return kMaxPostingKop;
    }
    return static_cast<Kopecks>(*value);
}

Kopecks compute_holding_income(const ProductionRules& rules,
                               ProvinceKind kind,
                               Kopecks value_kop,
                               Breakdown* breakdown) {
    const std::int32_t share = rules.owner_share_permille[static_cast<std::size_t>(kind_index(kind))];
    const Kopecks income = mul_permille(value_kop, share);
    if (breakdown != nullptr) {
        breakdown->begin(term::kOutputIncome, Unit::Kopecks, income);
        (void)breakdown->add_term(kRootNode, term::kOutputValue, value_kop);
        if (share != kPermilleOne) {
            (void)breakdown->add_step(kRootNode, term::kOutputOwnerShare, value_kop, income, share);
        }
        PAT_ASSERT(breakdown->is_consistent(), "the income breakdown must add up to the income");
    }
    return income;
}

// ---- F5 consumption, stock, wealth -----------------------------------------------------------------

Quantity compute_need(const ConsumptionRules& rules,
                      const std::array<std::uint32_t, kGroupCount>& sizes,
                      std::size_t good) noexcept {
    if (good >= kMaxGoods) {
        return 0;
    }
    // norms are thousandths of a unit per head: sum in milli-units, then to Q16.16 (truncated)
    std::int64_t milli = 0;
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        milli += static_cast<std::int64_t>(sizes[g]) * rules.need_per_head_milli[g][good];
    }
    return milli * kOne / 1000;
}

std::int32_t shortage_permille(Quantity need, Quantity available) noexcept {
    if (need <= 0) {
        return 0;
    }
    const Quantity deficit = std::max<Quantity>(0, need - std::max<Quantity>(0, available));
    return static_cast<std::int32_t>(std::min<Quantity>(kPermilleOne, deficit * kPermilleOne / need));
}

GrainResult compute_grain_stock(
    const ConsumptionRules& rules, Quantity need, Quantity stock, Quantity output, Breakdown* breakdown) {
    need = std::max<Quantity>(0, need);
    stock = std::max<Quantity>(0, stock);
    output = std::max<Quantity>(0, output);
    const Quantity available = saturating_add(stock, output);

    GrainResult out;
    out.consumed = std::min(need, available);
    out.shortage = shortage_permille(need, available);
    const Quantity remaining = available - out.consumed;
    const Quantity cap = std::min<Quantity>(kStockMax.raw(), saturating_mul(need, rules.stock_cap_months));
    out.new_stock = std::min(remaining, cap);
    out.discarded = remaining - out.new_stock;

    if (breakdown != nullptr) {
        breakdown->begin(term::kConsumptionStock, Unit::Pieces, out.new_stock);
        (void)breakdown->add_term(kRootNode, term::kConsumptionOpening, stock);
        (void)breakdown->add_term(kRootNode, term::kConsumptionOutput, output);
        (void)breakdown->add_term(kRootNode, term::kConsumptionNeed, -out.consumed);
        if (out.discarded > 0) {
            (void)breakdown->add_term(kRootNode, term::kConsumptionStockCap, -out.discarded);
        }
        PAT_ASSERT(breakdown->is_consistent(), "the stock breakdown must add up to the new stock");
    }
    return out;
}

WealthResult compute_wealth(const ConsumptionRules& rules,
                            Group group,
                            Fixed32 wealth,
                            const std::array<std::int32_t, kMaxGoods>& shortage,
                            Breakdown* breakdown) {
    const std::size_t g = index_of(group);
    PAT_ASSERT(g < kGroupCount, "wealth of an unknown group");
    const std::int64_t base = static_cast<std::int64_t>(rules.wealth_base[g]) * kOne;
    // sum of per-mille x points over the goods, then one conversion to Q16.16 points
    std::int64_t weighted = 0;
    for (std::size_t good = 0; good < kMaxGoods; ++good) {
        weighted += static_cast<std::int64_t>(shortage[good]) * rules.shortage_wealth_points[good];
    }
    const std::int64_t target =
        std::clamp<std::int64_t>(base - weighted * kOne / kPermilleOne, 0, kWealthMax.raw());

    if (breakdown != nullptr) {
        breakdown->begin(term::kWealthTarget, Unit::Points, target);
        (void)breakdown->add_term(kRootNode, term::kWealthBase, base);
        if (target != base) {
            (void)breakdown->add_term(kRootNode, term::kWealthShortage, target - base);
        }
        PAT_ASSERT(breakdown->is_consistent(), "the wealth breakdown must add up to the target");
    }

    // The gap closes by `inertia` per-mille a month (at least one raw unit, so wealth reaches its target;
    // never overshoots): the same rule as control and corruption, see `approach`.
    const Fixed32 target_value = Fixed32::from_raw(static_cast<std::int32_t>(target));
    const std::int64_t next = std::clamp<std::int64_t>(
        approach(wealth, target_value, rules.wealth_inertia_permille).raw(), 0, kWealthMax.raw());
    return WealthResult{target_value, Fixed32::from_raw(static_cast<std::int32_t>(next))};
}

// ---- province --------------------------------------------------------------------------------------

ProductionInput production_input(const World& world, const PriceSource& prices, ProvinceId province) {
    ProductionInput in;
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
    in.peasants_mood = e.groups.mood[first + index_of(Group::Peasants)];
    in.labour_base = e.provinces.labour_base[p];
    in.control = e.provinces.control[p];
    in.kind = static_cast<ProvinceKind>(e.provinces.kind[p]);
    in.grain_stock = e.provinces.grain_stock[p];
    for (std::size_t good = 0; good < kMaxGoods; ++good) {
        in.capacity[good] = e.provinces.capacity[p * kMaxGoods + good];
        if (in.capacity[good].raw() > 0) {
            in.price_kop[good] = std::clamp<Kopecks>(prices.price(world, province, good), 0, kMaxPostingKop);
        }
    }
    return in;
}

ProvinceProduction compute_province_production(const EconomyRules& rules, const ProductionInput& in) {
    ProvinceProduction out;
    out.labour_permille = labour_of(rules.production, in, nullptr);

    for (std::size_t good = 0; good < kMaxGoods; ++good) {
        if (in.capacity[good].raw() > 0) {
            out.output[good] = output_of(rules.production, in, out.labour_permille, good, nullptr);
            out.value_kop[good] = output_value(out.output[good], in.price_kop[good]);
            out.total_value_kop += out.value_kop[good];
        }
    }
    out.holding_income_kop = compute_holding_income(rules.production, in.kind, out.total_value_kop);

    for (std::size_t good = 0; good < kMaxGoods; ++good) {
        out.need[good] = compute_need(rules.consumption, in.size, good);
    }
    out.grain = compute_grain_stock(
        rules.consumption, out.need[kGrainGood], in.grain_stock.raw(), out.output[kGrainGood]);
    out.new_grain_stock = Fixed32::from_raw(static_cast<std::int32_t>(out.grain.new_stock));
    // Without trade a province can only eat what it makes in the month; grain also has the stock.
    for (std::size_t good = 0; good < kMaxGoods; ++good) {
        out.shortage_permille[good] = good == kGrainGood
                                          ? out.grain.shortage
                                          : economy::shortage_permille(out.need[good], out.output[good]);
    }
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        out.new_wealth[g] =
            compute_wealth(rules.consumption, static_cast<Group>(g), in.wealth[g], out.shortage_permille)
                .next;
    }
    return out;
}

void apply_province_production(World& world, ProvinceId province, const ProvinceProduction& production) {
    if (!world.economy_present() || !world.exists(province)) {
        return;
    }
    const EconomyColumnsMut columns = world.economy_columns_mut();
    columns.grain_stock[province.index()] = production.new_grain_stock;
    const std::size_t first = province.index() * kGroupCount;
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        columns.wealth[first + g] = production.new_wealth[g];
    }
}

ProvinceProduction step_province_production(World& world,
                                            const EconomyRules& rules,
                                            const PriceSource& prices,
                                            ProvinceId province) {
    if (!world.economy_present() || !world.exists(province)) {
        return {};
    }
    const ProvinceProduction production =
        compute_province_production(rules, production_input(world, prices, province));
    apply_province_production(world, province, production);
    return production;
}

// ---- explanations ----------------------------------------------------------------------------------

Breakdown explain_labour(const EconomyRules& rules, const ProductionInput& input) {
    Breakdown out;
    (void)labour_of(rules.production, input, &out);
    return out;
}

Breakdown explain_output(const EconomyRules& rules, const ProductionInput& input, std::size_t good) {
    Breakdown out;
    if (good < kMaxGoods) {
        const std::int32_t labour = labour_of(rules.production, input, nullptr);
        (void)output_of(rules.production, input, labour, good, &out);
    }
    return out;
}

Breakdown explain_holding_income(const EconomyRules& rules, const ProductionInput& input) {
    Breakdown out;
    const ProvinceProduction production = compute_province_production(rules, input);
    (void)compute_holding_income(rules.production, input.kind, production.total_value_kop, &out);
    return out;
}

Breakdown explain_grain_stock(const EconomyRules& rules, const ProductionInput& input) {
    Breakdown out;
    const ProvinceProduction production = compute_province_production(rules, input);
    (void)compute_grain_stock(rules.consumption,
                              production.need[kGrainGood],
                              input.grain_stock.raw(),
                              production.output[kGrainGood],
                              &out);
    return out;
}

Breakdown explain_wealth(const EconomyRules& rules, const ProductionInput& input, Group group) {
    Breakdown out;
    const std::size_t g = index_of(group);
    if (g < kGroupCount) {
        const ProvinceProduction production = compute_province_production(rules, input);
        (void)compute_wealth(rules.consumption, group, input.wealth[g], production.shortage_permille, &out);
    }
    return out;
}

}  // namespace pat::sim::economy
