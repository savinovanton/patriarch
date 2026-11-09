#include "pat/sim/economy/systems.hpp"

#include "pat/core/assert.hpp"
#include "pat/sim/calendar.hpp"
#include "pat/sim/economy/population.hpp"
#include "pat/sim/economy/tables.hpp"

#include <cstdint>
#include <memory>
#include <utility>

namespace pat::sim::economy {

namespace {

class ProvincesSystem final : public System {
public:
    explicit ProvincesSystem(const EconomyRules& rules) : rules_(rules) {}

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
        // Step 8: the slot is done. (The population cache was refreshed by the World with the sizes.)
        PAT_ASSERT(world.economy().provinces.last_slot_date[province.index()] != today,
                   "a province must be processed once per month");
        world.economy_columns_mut().last_slot_date[province.index()] = today;
    }

    EconomyRules rules_;
};

}  // namespace

std::unique_ptr<System> make_provinces_system(const EconomyRules& rules) {
    return std::make_unique<ProvincesSystem>(rules);
}

void install_economy_systems(Simulation& simulation, const EconomyRules& rules) {
    simulation.add_system(make_provinces_system(rules));
}

}  // namespace pat::sim::economy
