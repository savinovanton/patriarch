#pragma once

#include "pat/core/fixed.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/money.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/economy/types.hpp"
#include "pat/sim/entity_ids.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

// Production and consumption of a province (E9-T07, docs/dev/economy-model.md section 9 formulas
// F2-F5 and section 7.3 step 3, GDD 03 sections 5.2 and 5.3).
//
// Everything below the "province" line is a pure function of its arguments: integer-only, no random
// numbers, no World. The World is touched in exactly two places - `production_input` reads the state
// of one province into a plain struct and `apply_province_production` writes the result back - so
// the formulas can be tested, explained (Breakdown) and replaced without a world.
//
//   F2 labour   labour = clamp(min, max, workers x 1000 / labour_base)   (per-mille)
//               labour x= (1000 - max(0, -mood_peasants) x slope) / 1000
//   F3 output   output = capacity x labour x buildings x control/100 x (1 - losses) x harvest
//               (every factor is a per-mille multiplier applied in this order and truncated toward
//               zero; the buildings factor and the harvest are 1000 in M1: the first is the hook of
//               E9-T17, the second of the harvest roll of M2)
//   F4 value    value = output x price(province, good); holding income = value x owner share of the
//               kind of the province. The price comes from a PriceSource (E19 will plug the prices
//               of trade nodes in; M1 uses the static prices of the rules)
//   F5 stock    need = sum over groups of size x norm; grain: stock' = clamp(0, cap, stock + output
//               - need), cap = stock_cap_months x need; shortage = unmet share of the need (per-mille)
//               wealth target = base - sum over goods of shortage x points; wealth closes
//               wealth_inertia of the gap to the target each month
//
// Quantities of goods are `Quantity`: int64 in the raw units of Fixed32 (Q16.16, 65536 = one unit),
// so they share the resolution of the stock and capacity columns but are not squeezed into 32 bits
// on the way. Money is Kopecks. A province without trade (E19) can only consume what it makes: for
// every good except grain the available amount of a month is its own output, there is no stock.
//
// Consequences of a shortage for the mood reach the mood through wealth: F8 (E9-T11) has a wealth
// term, so a missing grain supply lowers wealth here and the mood there.

namespace pat::sim {
class World;
}

namespace pat::sim::economy {

/// Amount of a good: int64 raw Q16.16 units.
using Quantity = std::int64_t;
inline constexpr Quantity kQuantityOne = core::Fixed32::one_raw;
/// The goods slot of grain (the only good with a stock in M1; economy-model section 4).
inline constexpr std::size_t kGrainGood = 0;

// ---- prices --------------------------------------------------------------------------------------

/// Where the price of a good comes from (E9-T07; E19 replaces the implementation with prices of trade
/// nodes). The result must be a deterministic function of the world and the arguments; it is read
/// once per produced good when the input of a province is gathered. Values are clamped to
/// [0, kMaxPostingKop] by the caller.
class PriceSource {
public:
    virtual ~PriceSource() = default;
    /// Price in kopecks of one unit of `good` (a goods slot, < kMaxGoods) made in `province`.
    [[nodiscard]] virtual Kopecks price(const World& world, ProvinceId province, std::size_t good) const = 0;
};

/// The static prices of the rules (`ProductionRules::base_price_kop`), the same everywhere.
class StaticPriceSource final : public PriceSource {
public:
    explicit StaticPriceSource(const ProductionRules& rules) : prices_(rules.base_price_kop) {}
    [[nodiscard]] Kopecks price(const World& world, ProvinceId province, std::size_t good) const override;

private:
    std::array<Kopecks, kMaxGoods> prices_{};
};

// ---- the formulas --------------------------------------------------------------------------------

/// control (0..100 points) as per-mille of full control, truncated, clamped to 0..1000.
[[nodiscard]] std::int32_t control_permille(core::Fixed32 control) noexcept;

/// F2: the labour factor in per-mille. `workers` are the peasants and townsfolk now, `labour_base`
/// the workers at the start (0 means "no reference": the ratio is 1000). Root of the breakdown: code
/// term::kOutputLabour (Permille) = the factor; children: kOutputLabourWorkforce (the ratio after
/// clamping, `factor_permille` = the ratio before) and kOutputLabourMood (a step, left out at 0).
[[nodiscard]] std::int32_t compute_labour_permille(const ProductionRules& rules,
                                                   std::uint64_t workers,
                                                   std::uint32_t labour_base,
                                                   core::Fixed32 peasants_mood,
                                                   Breakdown* breakdown = nullptr);

/// F3: units of one good made in a month. Root: term::kOutputTotal (Pieces) = the output; children:
/// kOutputCapacity, then one step per multiplier (labour, buildings, control, losses, harvest) with
/// the multiplier in `factor_permille`; a multiplier of exactly 1000 is left out. Never negative.
[[nodiscard]] Quantity compute_output(const ProductionRules& rules,
                                      core::Fixed32 capacity,
                                      std::int32_t labour_permille,
                                      std::int32_t buildings_permille,
                                      core::Fixed32 control,
                                      std::int32_t harvest_permille,
                                      Breakdown* breakdown = nullptr);

/// F4: value of an output at a price, kopecks, truncated; capped at kMaxPostingKop (a posting limit).
[[nodiscard]] Kopecks output_value(Quantity output, Kopecks price_kop) noexcept;

/// F4: the share of a value that goes to the owner of a province of kind `kind`. Root:
/// term::kOutputIncome (Kopecks) = the income; children: kOutputValue and kOutputOwnerShare (a step).
[[nodiscard]] Kopecks compute_holding_income(const ProductionRules& rules,
                                             ProvinceKind kind,
                                             Kopecks value_kop,
                                             Breakdown* breakdown = nullptr);

/// F5: the monthly need of `good` of people of the groups, by the norms per head.
[[nodiscard]] Quantity compute_need(const ConsumptionRules& rules,
                                    const std::array<std::uint32_t, kGroupCount>& sizes,
                                    std::size_t good) noexcept;

/// F5: the share of `need` that `available` does not cover, per-mille, 0 when nothing is needed.
[[nodiscard]] std::int32_t shortage_permille(Quantity need, Quantity available) noexcept;

struct GrainResult {
    Quantity consumed = 0;      ///< eaten this month: min(need, stock + output)
    Quantity new_stock = 0;     ///< what stays in the granary, within 0..cap
    Quantity discarded = 0;     ///< surplus above the cap, lost
    std::int32_t shortage = 0;  ///< unmet share of the need, per-mille

    [[nodiscard]] bool operator==(const GrainResult&) const = default;
};

/// F5: one month of the grain stock. The cap is `stock_cap_months x need` (and never above the range
/// of the stock column), so a province without people keeps no grain. Root of the breakdown:
/// term::kConsumptionStock (Pieces) = the new stock; children: kConsumptionOpening (+), kConsumptionOutput
/// (+), kConsumptionNeed (- what was eaten), kConsumptionStockCap (- the surplus lost; left out at 0).
[[nodiscard]] GrainResult compute_grain_stock(const ConsumptionRules& rules,
                                              Quantity need,
                                              Quantity stock,
                                              Quantity output,
                                              Breakdown* breakdown = nullptr);

struct WealthResult {
    core::Fixed32 target;  ///< where the wealth of the group tends to this month
    core::Fixed32 next;    ///< the wealth after this month, within 0..kWealthMax
    [[nodiscard]] bool operator==(const WealthResult&) const = default;
};

/// F5: one month of the wealth of a group. Root of the breakdown: term::kWealthTarget (Points, Fixed32
/// raw) = the target; children: kWealthBase and kWealthShortage (what is missing, negative, left out
/// at 0). The buildings term (kWealthBuildings) is 0 in M1 and left out.
[[nodiscard]] WealthResult compute_wealth(const ConsumptionRules& rules,
                                          Group group,
                                          core::Fixed32 wealth,
                                          const std::array<std::int32_t, kMaxGoods>& shortage_permille,
                                          Breakdown* breakdown = nullptr);

// ---- province ------------------------------------------------------------------------------------

/// Everything the formulas read about one province, as plain values.
struct ProductionInput {
    std::array<std::uint32_t, kGroupCount> size{};
    core::Fixed32 peasants_mood;
    std::uint32_t labour_base = 0;
    core::Fixed32 control = kDefaultControl;
    ProvinceKind kind = ProvinceKind::Rural;
    std::array<core::Fixed32, kMaxGoods> capacity{};
    /// Multiplier of the buildings per good: 1000 in M1 (E9-T17 fills it from the modifier cache).
    std::array<std::int32_t, kMaxGoods> buildings_permille = [] {
        std::array<std::int32_t, kMaxGoods> ones{};
        ones.fill(kPermilleOne);
        return ones;
    }();
    /// 1000 in M1 (the harvest of M2).
    std::int32_t harvest_permille = kPermilleOne;
    /// Price of a unit of every good, as the PriceSource said (0 where nothing is produced).
    std::array<Kopecks, kMaxGoods> price_kop{};
    core::Fixed32 grain_stock;
    std::array<core::Fixed32, kGroupCount> wealth{};
};

/// The result of a month of production and consumption of one province.
struct ProvinceProduction {
    std::int32_t labour_permille = 0;
    std::array<Quantity, kMaxGoods> output{};
    std::array<Kopecks, kMaxGoods> value_kop{};
    Kopecks total_value_kop = 0;
    /// What the owner gets of the value (the money is posted by the taxes step, E9-T08).
    Kopecks holding_income_kop = 0;
    std::array<Quantity, kMaxGoods> need{};
    std::array<std::int32_t, kMaxGoods> shortage_permille{};
    GrainResult grain;
    core::Fixed32 new_grain_stock;
    std::array<core::Fixed32, kGroupCount> new_wealth{};

    [[nodiscard]] bool operator==(const ProvinceProduction&) const = default;
};

/// The input of a province as the World holds it; the prices come from `prices` (queried for every
/// good the province can produce). A default input for a world without economy or an unknown province.
[[nodiscard]] ProductionInput
production_input(const World& world, const PriceSource& prices, ProvinceId province);

/// One month of the province, from the input alone. Pure.
[[nodiscard]] ProvinceProduction compute_province_production(const EconomyRules& rules,
                                                             const ProductionInput& input);

/// Writes the grain stock and the wealth of the groups into the World; nothing else changes (output
/// that is not grain has no stock in M1, the income is posted by E9-T08).
void apply_province_production(World& world, ProvinceId province, const ProvinceProduction& production);

/// production_input + compute_province_production + apply_province_production: what the system
/// `economy.provinces` runs on the slot day of a province (step 3 of section 7.3).
ProvinceProduction step_province_production(World& world,
                                            const EconomyRules& rules,
                                            const PriceSource& prices,
                                            ProvinceId province);

// ---- explanations (the same code path as the simulation) ------------------------------------------

[[nodiscard]] Breakdown explain_labour(const EconomyRules& rules, const ProductionInput& input);
[[nodiscard]] Breakdown
explain_output(const EconomyRules& rules, const ProductionInput& input, std::size_t good);
[[nodiscard]] Breakdown explain_holding_income(const EconomyRules& rules, const ProductionInput& input);
[[nodiscard]] Breakdown explain_grain_stock(const EconomyRules& rules, const ProductionInput& input);
[[nodiscard]] Breakdown explain_wealth(const EconomyRules& rules, const ProductionInput& input, Group group);

}  // namespace pat::sim::economy
