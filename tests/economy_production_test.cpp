#include "pat/core/rng.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/economy/breakdown.hpp"
#include "pat/sim/economy/population.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/rules.hpp"
#include "pat/sim/economy/systems.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

// E9-T07: production, consumption, the grain stock and wealth (docs/dev/economy-model.md section 9,
// formulas F2-F5; GDD 03 sections 5.2 and 5.3).

using namespace pat::sim;
using namespace pat::sim::economy;
using pat::core::Fixed32;
using pat::core::Pcg32;
using pat::core::StringId;

namespace {

const EconomyRules& rules() {
    static const EconomyRules value = [] {
        EconomyRules r = EconomyRules::defaults();
        // E9-T10: the administration is frozen here, control and corruption stay as the test sets them
        // (the dynamics of F7 have their own tests in economy_administrator_test.cpp)
        r.control.control_inertia_permille = 0;
        r.control.corruption_inertia_permille = 0;
        // E9-T11: mood, unrest and riots are frozen too (their dynamics have their own tests)
        r.mood.inertia_permille = 0;
        r.unrest.inertia_permille = 0;
        r.unrest.riot_chance_min_permille = 0;
        r.unrest.riot_chance_max_permille = 0;
        return r;
    }();
    return value;
}

constexpr Quantity units(std::int64_t whole) {
    return whole * kQuantityOne;
}

void require_valid(const World& world) {
    const auto problems = world.check_invariants();
    for (const auto& problem : problems) {
        MESSAGE(problem);
    }
    REQUIRE(problems.empty());
}

/// A world with `provinces` provinces of the economy; the people of every province are `peasants`
/// peasants and `townsfolk` townsfolk, the rest is empty.
World make_world(std::uint32_t provinces,
                 std::uint32_t peasants = 1000,
                 std::uint32_t townsfolk = 0,
                 Date start = Date::from_ymd(1702, 12, 31)) {
    World world(start, 5);
    (void)world.create_dynasty(StringId(1));
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.create_province(StringId(100 + p), DynastyId(0), Fixed32::from_int(1));
    }
    REQUIRE(world.enable_economy());
    for (std::uint32_t p = 0; p < provinces; ++p) {
        (void)world.set_group_size(ProvinceId(p), Group::Peasants, peasants);
        (void)world.set_group_size(ProvinceId(p), Group::Townsfolk, townsfolk);
    }
    require_valid(world);
    return world;
}

void set_capacity(World& world, std::uint32_t province, std::size_t good, Quantity capacity) {
    world.economy_columns_mut().capacity[province * kMaxGoods + good] =
        Fixed32::from_raw(static_cast<std::int32_t>(capacity));
}

/// Capacity of grain that exactly feeds the people of a province at full labour and control.
Quantity grain_capacity_for(const World& world, std::uint32_t province, std::int64_t spare_permille = 1000) {
    std::array<std::uint32_t, kGroupCount> sizes{};
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        sizes[g] = world.economy().groups.size[group_row(province, static_cast<Group>(g))];
    }
    const Quantity need = compute_need(rules().consumption, sizes, kGrainGood);
    return need * spare_permille / (1000 - rules().production.losses_permille);
}

Simulation economy_sim(World world, std::shared_ptr<const PriceSource> prices = nullptr) {
    Simulation sim(std::move(world));
    install_economy_systems(sim, rules(), std::move(prices));
    return sim;
}

/// Runs to the end of the next calendar month (the world starts on the last day of a month).
void run_months(Simulation& sim, int months) {
    for (int i = 0; i < months; ++i) {
        const Date next = sim.world().today() + 1;
        sim.run_days(Date::days_in_month(next.year(), next.month()));
    }
}

const StaticPriceSource& static_prices() {
    static const StaticPriceSource value(rules().production);
    return value;
}

/// A price that differs by province and good, to see that the source is really asked.
class LocalPrices final : public PriceSource {
public:
    [[nodiscard]] Kopecks price(const World&, ProvinceId province, std::size_t good) const override {
        return 100 + 10 * static_cast<Kopecks>(province.index()) + static_cast<Kopecks>(good);
    }
};

/// Remembers every question.
class CountingPrices final : public PriceSource {
public:
    [[nodiscard]] Kopecks price(const World&, ProvinceId province, std::size_t good) const override {
        asked.emplace_back(province.index(), good);
        return 500;
    }
    mutable std::vector<std::pair<std::size_t, std::size_t>> asked;
};

}  // namespace

// ---- F2 labour -----------------------------------------------------------------------------------

TEST_CASE("production: control as per-mille, truncated and clamped") {
    CHECK(control_permille(Fixed32::from_int(100)) == 1000);
    CHECK(control_permille(Fixed32::from_int(0)) == 0);
    CHECK(control_permille(Fixed32::from_int(40)) == 400);
    CHECK(control_permille(Fixed32::from_raw(Fixed32::one_raw / 2)) ==
          5);  // half a point: 0.5 per-mille x 10
    CHECK(control_permille(Fixed32::from_raw(Fixed32::one_raw / 20)) == 0);
    CHECK(control_permille(Fixed32::from_int(-5)) == 0);
    CHECK(control_permille(Fixed32::from_int(250)) == 1000);
}

TEST_CASE("production: the labour factor is the workforce against its base, clamped, then the mood") {
    const ProductionRules& r = rules().production;
    const auto labour = [&](std::uint64_t workers, std::uint32_t base, std::int32_t mood = 0) {
        return compute_labour_permille(r, workers, base, Fixed32::from_int(mood));
    };
    CHECK(labour(1000, 1000) == 1000);
    CHECK(labour(1200, 1000) == 1200);
    CHECK(labour(1999, 1000) == 1500);  // the ceiling
    CHECK(labour(100, 1000) == 300);    // the floor
    CHECK(labour(0, 1000) == 300);
    CHECK(labour(1234, 1000) == 1234);
    CHECK(labour(1, 3) == 333);  // truncated toward zero
    // a base of 0 means "no reference": the ratio is 1, whatever the workers
    CHECK(labour(0, 0) == 1000);
    CHECK(labour(777, 0) == 1000);
    // each point of bad mood costs `slope` per-mille; good mood adds nothing
    CHECK(labour(1000, 1000, -10) == 950);
    CHECK(labour(1200, 1000, -10) == 1140);
    CHECK(labour(1000, 1000, 40) == 1000);
    CHECK(labour(1000, 1000, -100) == 500);
    // half a point of bad mood: 2.5 per-mille, truncated to 2
    CHECK(compute_labour_permille(r, 1000, 1000, Fixed32::from_raw(-Fixed32::one_raw / 2)) == 998);
    // the mood is applied after the clamp, so it can take the factor below the floor
    CHECK(labour(0, 1000, -100) == 150);
    // a huge workforce does not overflow
    CHECK(labour(std::uint64_t{1} << 33, 1) == 1500);
}

TEST_CASE("production: the labour breakdown adds up and names its terms") {
    Breakdown b;
    const std::int32_t factor =
        compute_labour_permille(rules().production, 1200, 1000, Fixed32::from_int(-10), &b);
    REQUIRE_FALSE(b.truncated());
    CHECK(b.is_consistent());
    CHECK(b.root().code == term::kOutputLabour);
    CHECK(b.root().unit == Unit::Permille);
    CHECK(b.total() == factor);
    CHECK(b.node(b.find(term::kOutputLabourWorkforce)).value == 1200);
    CHECK(b.node(b.find(term::kOutputLabourWorkforce)).factor_permille == 1200);
    CHECK(b.node(b.find(term::kOutputLabourMood)).value == -60);
    CHECK(b.node(b.find(term::kOutputLabourMood)).factor_permille == 950);

    // the ratio before the clamp is kept for the label; a good mood leaves no mood term
    Breakdown clamped;
    (void)compute_labour_permille(rules().production, 100, 1000, Fixed32::from_int(5), &clamped);
    CHECK(clamped.node(clamped.find(term::kOutputLabourWorkforce)).value == 300);
    CHECK(clamped.node(clamped.find(term::kOutputLabourWorkforce)).factor_permille == 100);
    CHECK(clamped.find(term::kOutputLabourMood) == kNoNode);
    CHECK(clamped.is_consistent());

    CHECK(compute_labour_permille(rules().production, 1200, 1000, Fixed32::from_int(-10)) == factor);
}

// ---- F3 output -----------------------------------------------------------------------------------

TEST_CASE("production: output is the capacity times five multipliers, by the terms of the GDD") {
    const ProductionRules& r = rules().production;  // losses 50 per-mille
    const Fixed32 capacity = Fixed32::from_int(1000);
    Breakdown b;
    // capacity 1000 x labour 1.2 x buildings 1.0 x control 0.8 x (1 - 0.05) x harvest 1.0 = 912
    const Quantity output = compute_output(r, capacity, 1200, 1000, Fixed32::from_int(80), 1000, &b);
    CHECK(output == units(912));

    REQUIRE_FALSE(b.truncated());
    CHECK(b.is_consistent());
    CHECK(b.root().code == term::kOutputTotal);
    CHECK(b.root().unit == Unit::Pieces);
    CHECK(b.total() == output);
    CHECK(b.node(b.find(term::kOutputCapacity)).value == units(1000));
    const BreakdownNode labour = b.node(b.find(term::kOutputLabour));
    CHECK(labour.value == units(200));
    CHECK(labour.factor_permille == 1200);
    CHECK(b.find(term::kOutputBuildings) == kNoNode);  // x1.0 steps are left out
    CHECK(b.find(term::kOutputHarvest) == kNoNode);
    const BreakdownNode control = b.node(b.find(term::kOutputControl));
    CHECK(control.value == -units(240));  // 1200 -> 960
    CHECK(control.factor_permille == 800);
    const BreakdownNode losses = b.node(b.find(term::kOutputLosses));
    CHECK(losses.value == -units(48));  // 960 -> 912
    CHECK(losses.factor_permille == 950);

    // the result does not depend on whether a breakdown was asked for
    CHECK(compute_output(r, capacity, 1200, 1000, Fixed32::from_int(80), 1000) == output);
}

TEST_CASE("production: every multiplier acts, in order, truncating toward zero") {
    const ProductionRules& r = rules().production;
    const Fixed32 capacity = Fixed32::from_int(100);
    const auto at =
        [&](std::int32_t labour, std::int32_t buildings, std::int32_t control_points, std::int32_t harvest) {
            return compute_output(r, capacity, labour, buildings, Fixed32::from_int(control_points), harvest);
        };
    CHECK(at(1000, 1000, 100, 1000) == units(95));  // only the losses
    CHECK(at(1500, 1000, 100, 1000) == units(142) + units(1) / 2);
    CHECK(at(1000, 1100, 100, 1000) == units(104) + units(1) / 2);  // 95 x 1.1 = 104.5
    CHECK(at(1000, 1000, 50, 1000) == units(47) + units(1) / 2);
    CHECK(at(1000, 1000, 100, 500) == units(47) + units(1) / 2);  // the harvest of M2
    CHECK(at(0, 1000, 100, 1000) == 0);
    CHECK(at(1000, 1000, 0, 1000) == 0);  // no control, no output
    // every step truncates: 7 raw units x 0.95 = 6.65 -> 6
    CHECK(compute_output(r, Fixed32::from_raw(7), 1000, 1000, Fixed32::from_int(100), 1000) == 6);
    // the order shows: 3 x 0.5 = 1 (truncated), then x 0.95 = 0; all at once it would be 1.425 -> 1
    CHECK(compute_output(r, Fixed32::from_raw(3), 500, 1000, Fixed32::from_int(100), 1000) == 0);
}

TEST_CASE("production: capacity, output and the other multipliers are never negative") {
    const ProductionRules& r = rules().production;
    CHECK(compute_output(r, Fixed32::from_int(-50), 1000, 1000, Fixed32::from_int(100), 1000) == 0);
    CHECK(compute_output(r, Fixed32::from_int(50), -300, 1000, Fixed32::from_int(100), 1000) == 0);
    CHECK(compute_output(r, Fixed32::from_int(50), 1000, 1000, Fixed32::from_int(-20), 1000) == 0);
    Pcg32 rng(77, 3);
    for (int i = 0; i < 5000; ++i) {
        const Quantity out =
            compute_output(r,
                           Fixed32::from_raw(rng.range(-1000, 32767 * Fixed32::one_raw)),
                           rng.range(-100, 1500),
                           rng.range(0, 3000),
                           Fixed32::from_raw(rng.range(-5 * Fixed32::one_raw, 120 * Fixed32::one_raw)),
                           rng.range(0, 1500));
        REQUIRE(out >= 0);
    }
}

// ---- F4 value and holding income ---------------------------------------------------------------------

TEST_CASE("production: the value of an output is output times price, truncated") {
    CHECK(output_value(units(912), 1200) == 1'094'400);
    CHECK(output_value(units(1), 1200) == 1200);
    CHECK(output_value(units(1) / 2, 1201) == 600);  // 600.5 -> 600
    CHECK(output_value(2293, 1200) == 41);           // 41.99... -> 41
    CHECK(output_value(1, 1200) == 0);
    CHECK(output_value(0, 1200) == 0);
    CHECK(output_value(units(912), 0) == 0);
    CHECK(output_value(-units(5), 1200) == 0);
    CHECK(output_value(units(5), -1200) == 0);
    // a value beyond a posting is capped, however large the output and the price
    CHECK(output_value(units(32767) * 100, kMaxPostingKop) == kMaxPostingKop);
    // a price beyond a posting counts as the largest posting
    CHECK(output_value(units(1) / 2, kMaxPostingKop * 4) == kMaxPostingKop / 2);
    CHECK(output_value(units(10), kMaxPostingKop * 4) == kMaxPostingKop);
}

TEST_CASE("production: the holding income is the owner's share of the value, per kind") {
    EconomyRules custom = rules();
    custom.production.owner_share_permille[index_of(ProvinceKind::City)] = 700;
    CHECK(compute_holding_income(custom.production, ProvinceKind::Rural, 1'094'400) == 1'094'400);
    CHECK(compute_holding_income(custom.production, ProvinceKind::City, 1'094'400) == 766'080);
    CHECK(compute_holding_income(custom.production, ProvinceKind::City, 1) == 0);  // truncated
    CHECK(compute_holding_income(custom.production, ProvinceKind::City, 0) == 0);

    Breakdown b;
    const Kopecks income = compute_holding_income(custom.production, ProvinceKind::City, 1'094'400, &b);
    REQUIRE_FALSE(b.truncated());
    CHECK(b.is_consistent());
    CHECK(b.root().code == term::kOutputIncome);
    CHECK(b.total() == income);
    CHECK(b.node(b.find(term::kOutputValue)).value == 1'094'400);
    CHECK(b.node(b.find(term::kOutputOwnerShare)).value == -328'320);
    CHECK(b.node(b.find(term::kOutputOwnerShare)).factor_permille == 700);

    // with the whole value going to the owner there is no share term
    Breakdown whole;
    (void)compute_holding_income(custom.production, ProvinceKind::Rural, 500, &whole);
    CHECK(whole.size() == 2);
    CHECK(whole.is_consistent());
}

// ---- F5 consumption ------------------------------------------------------------------------------

TEST_CASE("production: the need is the people of the groups times the norms per head") {
    const ConsumptionRules& r = rules().consumption;
    std::array<std::uint32_t, kGroupCount> sizes{};
    sizes[index_of(Group::Peasants)] = 1000;
    CHECK(compute_need(r, sizes, kGrainGood) == units(35));
    CHECK(compute_need(r, sizes, 1) == units(3));  // salt
    CHECK(compute_need(r, sizes, 7) == units(2));  // cloth
    CHECK(compute_need(r, sizes, 2) == 0);         // fur is not consumed
    CHECK(compute_need(r, sizes, kMaxGoods) == 0);

    sizes[index_of(Group::Nobility)] = 100;
    sizes[index_of(Group::Cossacks)] = 250;
    // 100 x 40 + 1000 x 35 + 250 x 38 = 48 500 milli-units
    CHECK(compute_need(r, sizes, kGrainGood) == 48'500 * kQuantityOne / 1000);
    // salt 3 each: 1350 x 3 = 4050 milli; cloth 100 x 6 + 1000 x 2 + 250 x 3 = 3350 milli
    CHECK(compute_need(r, sizes, 1) == 4050 * kQuantityOne / 1000);
    CHECK(compute_need(r, sizes, 7) == 3350 * kQuantityOne / 1000);

    // one person: 35 milli-units is 2293.76 raw, truncated
    std::array<std::uint32_t, kGroupCount> one{};
    one[index_of(Group::Peasants)] = 1;
    CHECK(compute_need(r, one, kGrainGood) == 2293);
    // an empty province needs nothing
    CHECK(compute_need(r, {}, kGrainGood) == 0);
    // the people of the reserve slot eat nothing
    std::array<std::uint32_t, kGroupCount> natives{};
    natives[index_of(Group::Natives)] = 5000;
    CHECK(compute_need(r, natives, kGrainGood) == 0);
}

TEST_CASE("production: the shortage is the unmet share of the need, in per-mille") {
    CHECK(shortage_permille(units(100), units(100)) == 0);
    CHECK(shortage_permille(units(100), units(250)) == 0);
    CHECK(shortage_permille(units(100), units(80)) == 200);
    CHECK(shortage_permille(units(100), 0) == 1000);
    CHECK(shortage_permille(units(100), -units(5)) == 1000);  // a damaged state is still a shortage
    CHECK(shortage_permille(0, 0) == 0);                      // nobody needs anything
    CHECK(shortage_permille(0, units(5)) == 0);
    CHECK(shortage_permille(3, 2) == 333);  // truncated
}

TEST_CASE("production: the grain stock takes the surplus up to the cap and gives it back in a bad month") {
    const ConsumptionRules& r = rules().consumption;  // cap: 3 months of need
    // a fat month: 1000 harvested, 100 eaten, the cap is 300: 600 are lost
    GrainResult fat = compute_grain_stock(r, units(100), 0, units(1000));
    CHECK(fat.consumed == units(100));
    CHECK(fat.new_stock == units(300));
    CHECK(fat.discarded == units(600));
    CHECK(fat.shortage == 0);

    // the stock fills step by step below the cap
    GrainResult slow = compute_grain_stock(r, units(100), units(50), units(130));
    CHECK(slow.new_stock == units(80));
    CHECK(slow.discarded == 0);

    // a lean month: the stock is eaten before anybody goes hungry
    GrainResult lean = compute_grain_stock(r, units(100), units(60), units(30));
    CHECK(lean.consumed == units(90) + units(0));
    CHECK(lean.new_stock == 0);
    CHECK(lean.shortage == 100);  // 10 of 100 are missing
    CHECK(lean.consumed + lean.new_stock + lean.discarded == units(60) + units(30));

    // a month without grain at all
    GrainResult famine = compute_grain_stock(r, units(100), 0, 0);
    CHECK(famine.consumed == 0);
    CHECK(famine.shortage == 1000);
    CHECK(famine.new_stock == 0);

    // nobody needs anything, so nothing is stored (the cap is months of need)
    GrainResult empty = compute_grain_stock(r, 0, units(40), units(40));
    CHECK(empty.new_stock == 0);
    CHECK(empty.discarded == units(80));
    CHECK(empty.shortage == 0);

    // the cap never exceeds what the stock column can hold
    GrainResult huge = compute_grain_stock(r, units(30'000), 0, units(32'000) * 3);
    CHECK(huge.new_stock == kStockMax.raw());

    // exactly met: nothing left, nothing missing
    GrainResult exact = compute_grain_stock(r, units(100), units(40), units(60));
    CHECK(exact.new_stock == 0);
    CHECK(exact.shortage == 0);
}

TEST_CASE("production: the grain breakdown adds up to the new stock") {
    const ConsumptionRules& r = rules().consumption;
    Breakdown b;
    const GrainResult fat = compute_grain_stock(r, units(100), units(20), units(1000), &b);
    REQUIRE_FALSE(b.truncated());
    CHECK(b.is_consistent());
    CHECK(b.root().code == term::kConsumptionStock);
    CHECK(b.root().unit == Unit::Pieces);
    CHECK(b.total() == fat.new_stock);
    CHECK(b.node(b.find(term::kConsumptionOpening)).value == units(20));
    CHECK(b.node(b.find(term::kConsumptionOutput)).value == units(1000));
    CHECK(b.node(b.find(term::kConsumptionNeed)).value == -units(100));
    CHECK(b.node(b.find(term::kConsumptionStockCap)).value == -fat.discarded);

    Breakdown lean;
    (void)compute_grain_stock(r, units(100), units(60), units(30), &lean);
    CHECK(lean.is_consistent());
    CHECK(lean.find(term::kConsumptionStockCap) == kNoNode);  // nothing lost, no term
    CHECK(lean.total() == 0);
    CHECK(compute_grain_stock(r, units(100), units(20), units(1000)) == fat);
}

TEST_CASE("production: a missing good lowers the wealth target by the points of the rule") {
    const ConsumptionRules& r = rules().consumption;  // grain 60 points, wealth base 100, inertia 20 %
    std::array<std::int32_t, kMaxGoods> none{};
    std::array<std::int32_t, kMaxGoods> half{};
    half[kGrainGood] = 500;
    std::array<std::int32_t, kMaxGoods> total{};
    total[kGrainGood] = 1000;

    const auto at = [&](std::int32_t wealth, const std::array<std::int32_t, kMaxGoods>& shortage) {
        return compute_wealth(r, Group::Peasants, Fixed32::from_int(wealth), shortage);
    };
    // nothing missing: the target is the base and a group at the base stays there
    CHECK(at(100, none).target == Fixed32::from_int(100));
    CHECK(at(100, none).next == Fixed32::from_int(100));
    // half the grain missing: target 70, 20 % of the gap of 30 is closed: 94
    CHECK(at(100, half).target == Fixed32::from_int(70));
    CHECK(at(100, half).next == Fixed32::from_int(94));
    // no grain at all: target 40
    CHECK(at(100, total).target == Fixed32::from_int(40));
    CHECK(at(100, total).next == Fixed32::from_int(88));
    // the wealth recovers toward the base when the shortage ends, from either side
    CHECK(at(60, none).next == Fixed32::from_int(68));
    CHECK(at(160, none).next == Fixed32::from_int(148));
    // salt and cloth carry no points in the default rules: missing them changes nothing
    std::array<std::int32_t, kMaxGoods> salt{};
    salt[1] = 1000;
    salt[7] = 1000;
    CHECK(at(100, salt).target == Fixed32::from_int(100));
    // the points of other goods add up; below zero the target stops at zero
    EconomyRules custom = rules();
    custom.consumption.shortage_wealth_points[1] = 30;
    custom.consumption.shortage_wealth_points[7] = 50;
    std::array<std::int32_t, kMaxGoods> all{};
    all[kGrainGood] = 1000;
    all[1] = 1000;
    all[7] = 1000;
    CHECK(compute_wealth(custom.consumption, Group::Clergy, Fixed32::from_int(100), all).target ==
          Fixed32::zero());
    all[7] = 0;
    CHECK(compute_wealth(custom.consumption, Group::Clergy, Fixed32::from_int(100), all).target ==
          Fixed32::from_int(10));
    // even a tiny gap closes by at least one raw unit, without overshooting
    const WealthResult tiny =
        compute_wealth(r, Group::Peasants, Fixed32::from_raw(Fixed32::one_raw * 100 + 3), none);
    CHECK(tiny.next.raw() == Fixed32::one_raw * 100 + 2);
    const WealthResult tiny_up =
        compute_wealth(r, Group::Peasants, Fixed32::from_raw(Fixed32::one_raw * 100 - 3), none);
    CHECK(tiny_up.next.raw() == Fixed32::one_raw * 100 - 2);
}

TEST_CASE("production: wealth reaches its exact target from either side without crossing it") {
    for (const std::int32_t missing : {0, 500, 1000}) {
        std::array<std::int32_t, kMaxGoods> shortage{};
        shortage[kGrainGood] = missing;
        for (const std::int32_t start : {0, 160}) {
            Fixed32 wealth = Fixed32::from_int(start);
            const Fixed32 target =
                compute_wealth(rules().consumption, Group::Peasants, wealth, shortage).target;
            for (int month = 0; month < 200; ++month) {
                const Fixed32 next =
                    compute_wealth(rules().consumption, Group::Peasants, wealth, shortage).next;
                if (wealth < target) {
                    CHECK(next > wealth);
                    CHECK(next <= target);
                } else if (wealth > target) {
                    CHECK(next < wealth);
                    CHECK(next >= target);
                } else {
                    CHECK(next == target);
                }
                wealth = next;
            }
            INFO("grain shortage " << missing << ", start " << start);
            CHECK(wealth == target);
        }
    }
}

TEST_CASE("production: the wealth breakdown adds up and the wealth stays in its range") {
    const ConsumptionRules& r = rules().consumption;
    std::array<std::int32_t, kMaxGoods> half{};
    half[kGrainGood] = 500;
    Breakdown b;
    const WealthResult result = compute_wealth(r, Group::Townsfolk, Fixed32::from_int(100), half, &b);
    REQUIRE_FALSE(b.truncated());
    CHECK(b.is_consistent());
    CHECK(b.root().code == term::kWealthTarget);
    CHECK(b.root().unit == Unit::Points);
    CHECK(b.total() == result.target.raw());
    CHECK(b.node(b.find(term::kWealthBase)).value == Fixed32::from_int(100).raw());
    CHECK(b.node(b.find(term::kWealthShortage)).value == -Fixed32::from_int(30).raw());
    CHECK(b.find(term::kWealthBuildings) == kNoNode);  // zero in M1

    Breakdown plain;
    (void)compute_wealth(r, Group::Townsfolk, Fixed32::from_int(100), {}, &plain);
    CHECK(plain.size() == 2);
    CHECK(plain.is_consistent());

    Pcg32 rng(5, 9);
    for (int i = 0; i < 5000; ++i) {
        std::array<std::int32_t, kMaxGoods> shortage{};
        for (auto& value : shortage) {
            value = rng.range(0, 1000);
        }
        Breakdown any;
        const Fixed32 wealth = Fixed32::from_raw(rng.range(0, 300 * Fixed32::one_raw));
        const WealthResult w =
            compute_wealth(r, static_cast<Group>(rng.bounded(kGroupCount)), wealth, shortage, &any);
        REQUIRE(any.is_consistent());
        REQUIRE(w.next >= Fixed32::zero());
        REQUIRE(w.next <= kWealthMax);
        REQUIRE(w.target >= Fixed32::zero());
        REQUIRE(w.target <= kWealthMax);
        // the step goes toward the target and never past it
        const std::int64_t before = static_cast<std::int64_t>(wealth.raw()) - w.target.raw();
        const std::int64_t after = static_cast<std::int64_t>(w.next.raw()) - w.target.raw();
        REQUIRE(std::max(after, -after) <= std::max(before, -before));
        REQUIRE((after == 0 || (after > 0) == (before > 0)));
    }
}

// ---- the province ---------------------------------------------------------------------------------

TEST_CASE("production: the input is read from the world and the prices from the source") {
    World world = make_world(3, 800, 200);
    set_capacity(world, 1, kGrainGood, units(300));
    set_capacity(world, 1, 3, units(40));
    world.economy_columns_mut().control[1] = Fixed32::from_int(70);
    world.economy_columns_mut().labour_base[1] = 900;
    world.economy_columns_mut().grain_stock[1] = Fixed32::from_int(25);
    world.economy_columns_mut().mood[group_row(1, Group::Peasants)] = Fixed32::from_int(-12);
    world.economy_columns_mut().wealth[group_row(1, Group::Cossacks)] = Fixed32::from_int(133);

    const LocalPrices local;
    const ProductionInput in = production_input(world, local, ProvinceId(1));
    CHECK(in.size[index_of(Group::Peasants)] == 800);
    CHECK(in.size[index_of(Group::Townsfolk)] == 200);
    CHECK(in.labour_base == 900);
    CHECK(in.control == Fixed32::from_int(70));
    CHECK(in.grain_stock == Fixed32::from_int(25));
    CHECK(in.peasants_mood == Fixed32::from_int(-12));
    CHECK(in.wealth[index_of(Group::Cossacks)] == Fixed32::from_int(133));
    CHECK(in.capacity[kGrainGood] == Fixed32::from_raw(static_cast<std::int32_t>(units(300))));
    CHECK(in.buildings_permille[kGrainGood] == 1000);
    CHECK(in.harvest_permille == 1000);
    // prices only where something is made
    CHECK(in.price_kop[kGrainGood] == local.price(world, ProvinceId(1), kGrainGood));
    CHECK(in.price_kop[3] == local.price(world, ProvinceId(1), 3));
    CHECK(in.price_kop[1] == 0);

    // a world without economy and an unknown province give an empty input
    World plain(Date::from_ymd(1703, 1, 1), 1);
    (void)plain.create_province(StringId(1), DynastyId(), Fixed32::from_int(5));
    CHECK(production_input(plain, local, ProvinceId(0)).labour_base == 0);
    CHECK(production_input(world, local, ProvinceId(77)).size[0] == 0);
    CHECK(step_province_production(plain, rules(), local, ProvinceId(0)) == ProvinceProduction{});
    CHECK(step_province_production(world, rules(), local, ProvinceId(77)) == ProvinceProduction{});
}

TEST_CASE("production: a month of a province is the formulas in order, and applying it writes two columns") {
    World world = make_world(2, 1000, 200);
    set_capacity(world, 0, kGrainGood, units(50));
    set_capacity(world, 0, 3, units(20));  // timber
    world.economy_columns_mut().grain_stock[0] = Fixed32::from_int(10);
    const World before = world;

    const ProductionInput in = production_input(world, static_prices(), ProvinceId(0));
    const ProvinceProduction month = compute_province_production(rules(), in);
    CHECK(world.economy() == before.economy());  // computing writes nothing

    // labour: 1200 workers, base 0 ("no reference") -> 1000; control 100; losses 5 %
    CHECK(month.labour_permille == 1000);
    CHECK(month.output[kGrainGood] == units(50) * 950 / 1000);
    CHECK(month.output[3] == units(20) * 950 / 1000);
    CHECK(month.output[1] == 0);
    CHECK(month.value_kop[kGrainGood] ==
          output_value(month.output[kGrainGood], rules().production.base_price_kop[0]));
    CHECK(month.value_kop[3] == output_value(month.output[3], rules().production.base_price_kop[3]));
    CHECK(month.total_value_kop == month.value_kop[kGrainGood] + month.value_kop[3]);
    CHECK(month.holding_income_kop == month.total_value_kop);  // the owner keeps it all (default rules)
    // needs: 1000 x 35 + 200 x 38 = 42 600 milli = 42.6 units of grain; 1200 x 3 salt
    CHECK(month.need[kGrainGood] == 42'600 * kQuantityOne / 1000);
    CHECK(month.need[1] == 3600 * kQuantityOne / 1000);  // 3.6 units of salt
    // grain: 10 stock + 47.5 harvested - 42.6 eaten = 14.9, below the cap of 3 months
    CHECK(month.grain.shortage == 0);
    CHECK(month.new_grain_stock.raw() == units(10) + units(50) * 950 / 1000 - month.need[kGrainGood]);
    // salt: nothing is made, everything is missing - but it costs no wealth by the default rules
    CHECK(month.shortage_permille[1] == 1000);
    CHECK(month.shortage_permille[kGrainGood] == 0);
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        CHECK(month.new_wealth[g] == Fixed32::from_int(100));
    }

    apply_province_production(world, ProvinceId(0), month);
    CHECK(world.economy().provinces.grain_stock[0] == month.new_grain_stock);
    CHECK(world.economy().provinces.grain_stock[1] == before.economy().provinces.grain_stock[1]);
    // nothing but the stock and the wealth of that province changed
    World expected = before;
    expected.economy_columns_mut().grain_stock[0] = month.new_grain_stock;
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        expected.economy_columns_mut().wealth[group_row(0, static_cast<Group>(g))] = month.new_wealth[g];
    }
    CHECK(world.economy() == expected.economy());
    CHECK(world.compute_hash() == expected.compute_hash());
    require_valid(world);
}

TEST_CASE("production: a lack of grain lowers wealth month by month, enough grain keeps it") {
    World world = make_world(2, 1000, 200);
    set_capacity(world, 1, kGrainGood, grain_capacity_for(world, 1, 1200));  // province 1 feeds itself
    Simulation sim = economy_sim(std::move(world));

    // month by month: the hungry province (0) loses wealth toward 100 - 60 = 40, the other stays
    std::array<std::int32_t, 4> hungry{};
    for (int month = 0; month < 4; ++month) {
        run_months(sim, 1);
        hungry[static_cast<std::size_t>(month)] =
            sim.world().economy().groups.wealth[group_row(0, Group::Peasants)].raw();
    }
    // 100 -> 88 -> 78.4 -> 70.72: the first four months close 20 % of the gap to 40 each time
    CHECK(hungry[0] == Fixed32::from_int(88).raw());
    CHECK(hungry[1] < hungry[0]);
    CHECK(hungry[2] < hungry[1]);
    CHECK(hungry[3] < hungry[2]);
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        CHECK(sim.world().economy().groups.wealth[group_row(1, static_cast<Group>(g))] ==
              Fixed32::from_int(100));
    }
    run_months(sim, 24);
    const Fixed32 starving = sim.world().economy().groups.wealth[group_row(0, Group::Peasants)];
    CHECK(starving >= Fixed32::from_int(40));  // never below the target
    CHECK(starving < Fixed32::from_int(42));
    // no grain is ever conjured: the empty granary stays empty and nothing is negative
    CHECK(sim.world().economy().provinces.grain_stock[0] == Fixed32::zero());
    require_valid(sim.world());
}

TEST_CASE("production: a province that feeds itself fills its granary to three months of need") {
    World world = make_world(1, 1000, 200);
    set_capacity(world, 0, kGrainGood, grain_capacity_for(world, 0, 2000));  // twice what it eats
    Simulation sim = economy_sim(std::move(world));
    Fixed32 previous = Fixed32::zero();
    bool grew = false;
    for (int month = 0; month < 8; ++month) {
        run_months(sim, 1);
        const Fixed32 stock = sim.world().economy().provinces.grain_stock[0];
        CHECK(stock >= previous);  // a surplus only adds up
        grew = grew || stock > previous;
        previous = stock;
    }
    CHECK(grew);
    // the cap: three months of the need of the people as they are now (they grew a little)
    std::array<std::uint32_t, kGroupCount> sizes{};
    for (std::size_t g = 0; g < kGroupCount; ++g) {
        sizes[g] = sim.world().economy().groups.size[group_row(0, static_cast<Group>(g))];
    }
    const Quantity need = compute_need(rules().consumption, sizes, kGrainGood);
    CHECK(previous.raw() <= need * rules().consumption.stock_cap_months);
    CHECK(previous.raw() > need * 2);  // and it is near the cap after eight months
    CHECK(sim.world().economy().groups.wealth[group_row(0, Group::Peasants)] == Fixed32::from_int(100));
    require_valid(sim.world());
}

TEST_CASE("production: hunger slows the growth of the people through wealth") {
    World world = make_world(2, 5000, 500);
    set_capacity(world, 1, kGrainGood, grain_capacity_for(world, 1, 1200));
    Simulation sim = economy_sim(std::move(world));
    sim.run_days(3 * 365);
    const std::uint32_t hungry = sim.world().economy().groups.size[group_row(0, Group::Peasants)];
    const std::uint32_t fed = sim.world().economy().groups.size[group_row(1, Group::Peasants)];
    CHECK(fed > 5000);     // half a percent a year
    CHECK(hungry > 5000);  // wealth near 40 costs 1200 ppm of the 5000: still growing
    CHECK(hungry < fed);   // ... but slower
    require_valid(sim.world());
}

TEST_CASE("production: the labour of a province follows its workforce and the mood of its peasants") {
    World world = make_world(1, 1000, 0);
    const auto labour = [&](std::uint32_t base, std::int32_t mood) {
        world.economy_columns_mut().labour_base[0] = base;
        world.economy_columns_mut().mood[group_row(0, Group::Peasants)] = Fixed32::from_int(mood);
        const ProductionInput in = production_input(world, static_prices(), ProvinceId(0));
        return compute_province_production(rules(), in).labour_permille;
    };
    CHECK(labour(1000, 0) == 1000);
    CHECK(labour(500, 0) == 1500);  // twice the workers of the start, capped at x1.5
    CHECK(labour(2000, 0) == 500);  // half the workers
    CHECK(labour(1000, -20) == 900);
    CHECK(labour(0, -20) == 900);  // no base: ratio 1
}

TEST_CASE("production: random provinces keep every invariant of the numbers") {
    Pcg32 rng(2025, 8);
    for (int round = 0; round < 3000; ++round) {
        ProductionInput in;
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            in.size[g] = rng.chance_permille(300) ? 0 : rng.bounded(4'000'000);
            in.wealth[g] = Fixed32::from_raw(rng.range(0, 300 * Fixed32::one_raw));
        }
        in.peasants_mood = Fixed32::from_raw(rng.range(-100 * Fixed32::one_raw, 100 * Fixed32::one_raw));
        in.labour_base = rng.bounded(4'000'000);
        in.control = Fixed32::from_raw(rng.range(0, 100 * Fixed32::one_raw));
        in.kind = static_cast<ProvinceKind>(rng.bounded(kProvinceKindCount));
        in.grain_stock = Fixed32::from_raw(rng.range(0, 32767 * Fixed32::one_raw));
        for (std::size_t good = 0; good < kMaxGoods; ++good) {
            in.capacity[good] = Fixed32::from_raw(rng.range(0, 32767 * Fixed32::one_raw));
            in.price_kop[good] = rng.range(0, 5'000'000);
            in.buildings_permille[good] = rng.range(500, 2000);
        }
        const ProvinceProduction month = compute_province_production(rules(), in);
        REQUIRE(month == compute_province_production(rules(), in));
        Kopecks sum = 0;
        for (std::size_t good = 0; good < kMaxGoods; ++good) {
            REQUIRE(month.output[good] >= 0);
            REQUIRE(month.need[good] >= 0);
            REQUIRE(month.value_kop[good] >= 0);
            REQUIRE(month.value_kop[good] <= kMaxPostingKop);
            REQUIRE(month.shortage_permille[good] >= 0);
            REQUIRE(month.shortage_permille[good] <= 1000);
            sum += month.value_kop[good];
        }
        REQUIRE(month.total_value_kop == sum);
        REQUIRE(month.holding_income_kop >= 0);
        REQUIRE(month.holding_income_kop <= month.total_value_kop);
        REQUIRE(month.new_grain_stock >= Fixed32::zero());
        REQUIRE(month.new_grain_stock <= kStockMax);
        REQUIRE(month.grain.consumed + month.grain.new_stock + month.grain.discarded ==
                in.grain_stock.raw() + month.output[kGrainGood]);
        for (std::size_t g = 0; g < kGroupCount; ++g) {
            REQUIRE(month.new_wealth[g] >= Fixed32::zero());
            REQUIRE(month.new_wealth[g] <= kWealthMax);
        }
        // the explanations tell the same numbers
        const std::size_t good = rng.bounded(kMaxGoods);
        const Breakdown out = explain_output(rules(), in, good);
        REQUIRE(out.is_consistent());
        REQUIRE(out.total() == month.output[good]);
        REQUIRE(explain_labour(rules(), in).total() == month.labour_permille);
        REQUIRE(explain_holding_income(rules(), in).total() == month.holding_income_kop);
        REQUIRE(explain_grain_stock(rules(), in).total() == month.new_grain_stock.raw());
        const auto group = static_cast<Group>(rng.bounded(kGroupCount));
        const Breakdown wealth = explain_wealth(rules(), in, group);
        REQUIRE(wealth.is_consistent());
        REQUIRE(
            wealth.total() ==
            compute_wealth(rules().consumption, group, in.wealth[index_of(group)], month.shortage_permille)
                .target.raw());
    }
}

TEST_CASE("production: the explanations of a province match the month and an unknown index is empty") {
    World world = make_world(1, 1000, 200);
    set_capacity(world, 0, kGrainGood, units(50));
    const ProductionInput in = production_input(world, static_prices(), ProvinceId(0));
    const ProvinceProduction month = compute_province_production(rules(), in);
    CHECK(explain_output(rules(), in, kGrainGood).total() == month.output[kGrainGood]);
    CHECK(explain_output(rules(), in, kGrainGood).root().code == term::kOutputTotal);
    CHECK(explain_output(rules(), in, kMaxGoods).empty());
    CHECK(explain_wealth(rules(), in, Group::Peasants).root().code == term::kWealthTarget);
    CHECK(explain_wealth(rules(), in, static_cast<Group>(kGroupCount)).empty());
    CHECK(explain_labour(rules(), in).root().code == term::kOutputLabour);
    CHECK(explain_grain_stock(rules(), in).root().code == term::kConsumptionStock);
    CHECK(explain_holding_income(rules(), in).root().code == term::kOutputIncome);
}

// ---- PriceSource ----------------------------------------------------------------------------------

TEST_CASE("production: the price source is replaceable and decides the value, not the output") {
    World world = make_world(3, 1000, 200);
    for (std::uint32_t p = 0; p < 3; ++p) {
        set_capacity(world, p, kGrainGood, units(50));
        set_capacity(world, p, 5, units(10));  // iron
    }
    const LocalPrices local;
    for (std::uint32_t p = 0; p < 3; ++p) {
        const ProvinceId id(p);
        const ProvinceProduction fixed =
            compute_province_production(rules(), production_input(world, static_prices(), id));
        const ProvinceProduction varied =
            compute_province_production(rules(), production_input(world, local, id));
        // the same things are made and eaten, whatever the price
        CHECK(fixed.output == varied.output);
        CHECK(fixed.new_grain_stock == varied.new_grain_stock);
        CHECK(fixed.new_wealth == varied.new_wealth);
        // the static source is the table of the rules, the test source depends on the province
        CHECK(fixed.value_kop[kGrainGood] == output_value(fixed.output[kGrainGood], 1200));
        CHECK(fixed.value_kop[5] == output_value(fixed.output[5], 1000));
        CHECK(varied.value_kop[kGrainGood] ==
              output_value(varied.output[kGrainGood], local.price(world, id, kGrainGood)));
        CHECK(varied.value_kop[5] == output_value(varied.output[5], local.price(world, id, 5)));
        CHECK(varied.total_value_kop == varied.value_kop[kGrainGood] + varied.value_kop[5]);
    }
    CHECK(static_prices().price(world, ProvinceId(0), kGrainGood) ==
          rules().production.base_price_kop[kGrainGood]);
    CHECK(static_prices().price(world, ProvinceId(2), 7) == rules().production.base_price_kop[7]);
    CHECK(static_prices().price(world, ProvinceId(0), kMaxGoods) == 0);
}

TEST_CASE("production: a price outside the posting range is clamped when the input is gathered") {
    class Wild final : public PriceSource {
    public:
        [[nodiscard]] Kopecks price(const World&, ProvinceId province, std::size_t) const override {
            return province.index() == 0 ? -500 : 4 * kMaxPostingKop;
        }
    };
    World world = make_world(2);
    set_capacity(world, 0, kGrainGood, units(5));
    set_capacity(world, 1, kGrainGood, units(5));
    const Wild wild;
    CHECK(production_input(world, wild, ProvinceId(0)).price_kop[kGrainGood] == 0);
    CHECK(production_input(world, wild, ProvinceId(1)).price_kop[kGrainGood] == kMaxPostingKop);
}

TEST_CASE("economy.provinces: the system asks the injected price source once per good of a slot") {
    constexpr std::uint32_t kProvinces = 30;
    World world = make_world(kProvinces);
    for (std::uint32_t p = 0; p < kProvinces; ++p) {
        set_capacity(world, p, kGrainGood, units(20));
        if (p % 2 == 0) {
            set_capacity(world, p, 7, units(5));
        }
    }
    auto prices = std::make_shared<CountingPrices>();
    Simulation sim = economy_sim(std::move(world), prices);
    run_months(sim, 1);  // January: every province has had its slot
    std::size_t expected = 0;
    for (std::uint32_t p = 0; p < kProvinces; ++p) {
        expected += p % 2 == 0 ? 2 : 1;
    }
    CHECK(prices->asked.size() == expected);
    // the questions are the producing goods of the provinces, in ascending order of province
    CHECK(prices->asked.front() == std::pair<std::size_t, std::size_t>{0, kGrainGood});
    for (const auto& [province, good] : prices->asked) {
        CHECK(good < kMaxGoods);
        CHECK((good == kGrainGood || (good == 7 && province % 2 == 0)));
    }
    // a second month asks again, once per slot
    run_months(sim, 1);
    CHECK(prices->asked.size() == 2 * expected);
}

// ---- the system, once a month -----------------------------------------------------------------------

TEST_CASE("economy.provinces: production runs once per province per month, in months of any length") {
    constexpr std::uint32_t kProvinces = 61;  // not a multiple of 28: the slots are not even
    // Julian leap years 1704, 1708 and the century year 1800: 28, 29, 30 and 31-day months
    for (const int first_year : {1703, 1799}) {
        World start = make_world(kProvinces, 3000, 500, Date::from_ymd(first_year - 1, 12, 31));
        for (std::uint32_t p = 0; p < kProvinces; ++p) {
            // provinces differ: some starve, some have a surplus, some make several goods
            const Quantity fraction =
                grain_capacity_for(start, p, 600 + 40 * static_cast<std::int64_t>(p % 20));
            set_capacity(start, p, kGrainGood, fraction);
            if (p % 3 == 0) {
                set_capacity(start, p, 7, units(30));
            }
            start.economy_columns_mut().labour_base[p] = 3200 + p;
            start.economy_columns_mut().grain_stock[p] = Fixed32::from_int(static_cast<std::int64_t>(p) * 3);
        }
        require_valid(start);

        // the model: for every month, for every province, growth then production, exactly once
        World model = start;
        Simulation sim = economy_sim(start);
        const int last_year = first_year == 1703 ? 1712 : 1800;
        std::array<int, 32> lengths{};
        for (int year = first_year; year <= last_year; ++year) {
            for (int month = 1; month <= 12; ++month) {
                const int days = Date::days_in_month(year, month);
                ++lengths[static_cast<std::size_t>(days)];
                run_months(sim, 1);
                REQUIRE(sim.world().today() == Date::from_ymd(year, month, days));
                for (std::uint32_t p = 0; p < kProvinces; ++p) {
                    (void)step_province_population(model, rules().population, ProvinceId(p));
                    (void)step_province_production(model, rules(), static_prices(), ProvinceId(p));
                }
                INFO("year " << year << " month " << month << " (" << days << " days)");
                REQUIRE(sim.world().economy().groups == model.economy().groups);
                REQUIRE(sim.world().economy().provinces.grain_stock == model.economy().provinces.grain_stock);
            }
        }
        CHECK(lengths[28] > 0);
        CHECK(lengths[29] > 0);
        CHECK(lengths[30] > 0);
        CHECK(lengths[31] > 0);
        require_valid(sim.world());
    }
}

TEST_CASE("economy.provinces: two runs of the same world agree and move the granary and the wealth") {
    World world = make_world(40, 2000, 300);
    for (std::uint32_t p = 0; p < 40; ++p) {
        set_capacity(
            world, p, kGrainGood, grain_capacity_for(world, p, 700 + 20 * static_cast<std::int64_t>(p)));
    }
    Simulation a = economy_sim(world);
    Simulation b = economy_sim(world);
    a.run_days(200);
    b.run_days(200);
    CHECK(a.world().compute_hash() == b.world().compute_hash());
    CHECK(a.world().economy() == b.world().economy());
    CHECK(a.world().economy().provinces.grain_stock != world.economy().provinces.grain_stock);
    CHECK(a.world().economy().groups.wealth != world.economy().groups.wealth);
    require_valid(a.world());
}
