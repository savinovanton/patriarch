#include "pat/sim/economy/systems.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/economy/population.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/economy/taxes.hpp"

#include <cstdint>
#include <memory>
#include <utility>

namespace pat::sim::economy {

namespace {

class ProvincesSystem final : public System {
public:
    ProvincesSystem(const EconomyRules& rules, std::shared_ptr<const PriceSource> prices)
        : rules_(rules),
          prices_(prices ? std::move(prices) : std::make_shared<const StaticPriceSource>(rules.production)) {}

    [[nodiscard]] std::string_view name() const override { return kProvincesSystemName; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }

    void run(World& world, TickContext& context) override {
        if (!world.economy_present()) {
            return;
        }
        const int day = context.date.day();
        if (day > kStaggerDays) {
            return;  // days 29..31 select nobody (is_monthly_slot)
        }
        const auto count = static_cast<std::uint32_t>(world.provinces().size());
        constexpr auto kStride = static_cast<std::uint32_t>(kStaggerDays);
        // Ascending province index: the order inside a day is part of the state hash (spec 7.2).
        for (auto index = static_cast<std::uint32_t>(day - 1); index < count; index += kStride) {
            PAT_ASSERT(is_monthly_slot(index, context.date), "stride and is_monthly_slot must agree");
            process_province(world, ProvinceId(index), context.date);
        }
    }

private:
    /// The steps of spec section 7.3 that exist so far, in their fixed order.
    void process_province(World& world, ProvinceId province, Date today) {
        // Step 2: growth of the population.
        (void)step_province_population(world, rules_.population, province);
        // Step 3: production and consumption (F2-F5); the grain stock and the wealth are written. Its
        // result carries the holding income that step 4 posts.
        const ProvinceProduction production = step_province_production(world, rules_, *prices_, province);
        // Step 4: taxes of the groups and the holding income go to the owner (and the crown's share of
        // a city to the crown) through post_money; `income_last_month` is written (F6, E9-T08).
        (void)step_province_taxes(world, rules_, province, production.holding_income_kop);
        // Step 8: the slot is done. (The population cache was refreshed by the World with the sizes.)
        PAT_ASSERT(world.economy().provinces.last_slot_date[province.index()] != today,
                   "a province must be processed once per month");
        world.economy_columns_mut().last_slot_date[province.index()] = today;
    }

    EconomyRules rules_;
    std::shared_ptr<const PriceSource> prices_;
};

}  // namespace

std::unique_ptr<System> make_provinces_system(const EconomyRules& rules,
                                              std::shared_ptr<const PriceSource> prices) {
    return std::make_unique<ProvincesSystem>(rules, std::move(prices));
}

void install_economy_systems(Simulation& simulation,
                             const EconomyRules& rules,
                             std::shared_ptr<const PriceSource> prices) {
    simulation.add_system(make_provinces_system(rules, std::move(prices)));
}

}  // namespace pat::sim::economy
