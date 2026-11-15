#include "pat/sim/demo_scenario.hpp"

#include <doctest/doctest.h>

#include <algorithm>

using namespace pat::sim;

TEST_CASE("demo growth: inactive month-end days create the RNG stream without drawing") {
    for (int day = 29; day <= 31; ++day) {
        CAPTURE(day);
        demo::Config config;
        config.dynasties = 0;
        config.provinces = 0;
        config.characters_per_dynasty = 0;
        Simulation simulation = demo::make_simulation(config);
        World world = simulation.world();
        const Date previous_day = Date::from_ymd(1703, 1, day - 1);
        while (world.today() < previous_day) {
            (void)world.advance_day();
        }

        pat::core::RngStreams expected_rng(config.seed);
        expected_rng.restore(world.rng().save());
        (void)expected_rng.stream("demo.growth");

        simulation.restore_state(world, {});
        const TickReport& report = simulation.tick();
        CHECK(report.date == Date::from_ymd(1703, 1, day));
        CHECK(simulation.world().rng().save() == expected_rng.save());
        const auto growth = std::find_if(report.systems.begin(),
                                         report.systems.end(),
                                         [](const SystemRun& run) { return run.name == "demo.growth"; });
        REQUIRE(growth != report.systems.end());
        CHECK(growth->ran);
        CHECK(growth->wu_used == 0);
    }
}
