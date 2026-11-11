#include "pat/sim/economy/systems.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/economy/administrator.hpp"
#include "pat/sim/economy/mood.hpp"
#include "pat/sim/economy/policy.hpp"
#include "pat/sim/economy/population.hpp"
#include "pat/sim/economy/production.hpp"
#include "pat/sim/economy/recruits.hpp"
#include "pat/sim/economy/settlement.hpp"
#include "pat/sim/economy/tables.hpp"
#include "pat/sim/economy/taxes.hpp"
#include "pat/sim/economy/unrest.hpp"

#include <cstdint>
#include <memory>
#include <utility>

namespace pat::sim::economy {

namespace {

class SettlementSystem final : public System {
public:
    SettlementSystem(const EconomyRules& rules, ExpenseSources expenses)
        : rules_(rules), expenses_(std::move(expenses)) {}

    [[nodiscard]] std::string_view name() const override { return kSettlementSystemName; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Monthly; }

    void run(World& world, TickContext& context) override {
        if (!world.economy_present()) {
            return;
        }
        (void)settle_month(
            world, rules_, expenses_, Date::from_ymd(context.date.year(), context.date.month(), 1));
    }

private:
    EconomyRules rules_;
    ExpenseSources expenses_;
};

class ProvincesSystem final : public System {
public:
    ProvincesSystem(const EconomyRules& rules,
                    std::shared_ptr<const PriceSource> prices,
                    std::shared_ptr<const AdministratorSource> administrators)
        : rules_(rules),
          prices_(prices ? std::move(prices) : std::make_shared<const StaticPriceSource>(rules.production)),
          administrators_(administrators ? std::move(administrators)
                                         : std::make_shared<const StubAdministratorSource>()) {}

    [[nodiscard]] std::string_view name() const override { return kProvincesSystemName; }
    [[nodiscard]] Cadence cadence() const override { return Cadence::Daily; }

    void run(World& world, TickContext& context) override {
        if (!world.economy_present()) {
            return;
        }
        bind_policy_levels(world, rules_);  // the range of the policy commands follows the rules (E9-T12)
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
        // Step 1: control and corruption move toward the targets of the administrator (F7, E9-T10);
        // the rest of the slot, taxes included, sees the new values.
        (void)step_province_administration(world, rules_, administrators_.get(), province);
        // Step 2: growth of the population.
        (void)step_province_population(world, rules_.population, province);
        // Step 3: production and consumption (F2-F5); the grain stock and the wealth are written. Its
        // result carries the holding income that step 4 posts.
        const ProvinceProduction production = step_province_production(world, rules_, *prices_, province);
        // Step 4: taxes of the groups and the holding income go to the owner (and the crown's share of
        // a city to the crown) through post_money; `income_last_month` is written (F6, E9-T08).
        const ProvinceIncome income =
            step_province_taxes(world, rules_, province, production.holding_income_kop);
        // A province in turmoil pays its owner nothing; the incident log says so (E9-T10).
        (void)record_anarchy(world, province, income.anarchy);
        // Step 5: the moods of the groups (F8), then the unrest of the province from the new moods (F9);
        // a change of state goes to the incident log (E9-T11).
        (void)step_province_mood(world, rules_, province);
        (void)step_province_unrest(world, rules_, province);
        // Step 6: the recruit pool regenerates toward the potential (F11, E9-T13).
        (void)step_province_recruits(world, rules_, province);
        // Step 7: the one draw of a province in unrest, the loot and the fall of control of a riot (F10).
        (void)step_province_riot(world, rules_, province);
        // Step 8: the slot is done. (The population cache was refreshed by the World with the sizes.)
        PAT_ASSERT(world.economy().provinces.last_slot_date[province.index()] != today,
                   "a province must be processed once per month");
        world.economy_columns_mut().last_slot_date[province.index()] = today;
    }

    EconomyRules rules_;
    std::shared_ptr<const PriceSource> prices_;
    std::shared_ptr<const AdministratorSource> administrators_;
};

}  // namespace

std::unique_ptr<System> make_settlement_system(const EconomyRules& rules, ExpenseSources expenses) {
    return std::make_unique<SettlementSystem>(rules, std::move(expenses));
}

std::unique_ptr<System> make_provinces_system(const EconomyRules& rules,
                                              std::shared_ptr<const PriceSource> prices,
                                              std::shared_ptr<const AdministratorSource> administrators) {
    return std::make_unique<ProvincesSystem>(rules, std::move(prices), std::move(administrators));
}

void install_economy_systems(Simulation& simulation,
                             const EconomyRules& rules,
                             std::shared_ptr<const PriceSource> prices,
                             ExpenseSources expenses,
                             std::shared_ptr<const AdministratorSource> administrators) {
    // The month is closed before the slots of its first day (spec section 7.1).
    simulation.add_system(make_settlement_system(rules, std::move(expenses)));
    simulation.add_system(make_provinces_system(rules, std::move(prices), std::move(administrators)));
}

}  // namespace pat::sim::economy
