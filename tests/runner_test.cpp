#include "pat/sim/demo_scenario.hpp"
#include "pat/sim/runner.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace pat::sim;
using namespace std::chrono_literals;

namespace {

demo::Config small_config(std::uint64_t seed = 1) {
    demo::Config c;
    c.seed = seed;
    c.dynasties = 4;
    c.provinces = 20;
    c.characters_per_dynasty = 4;
    return c;
}

}  // namespace

TEST_CASE("runner: a snapshot exists from the start and the runner begins paused") {
    SimulationRunner runner(demo::make_simulation(small_config()));
    const auto snapshot = runner.latest_snapshot();
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->today() == Date::from_ymd(1703, 1, 1));
    CHECK(runner.speed() == Speed::Paused);

    runner.start();
    std::this_thread::sleep_for(50ms);
    CHECK(runner.ticks_done() == 0);  // paused: nothing runs
    runner.stop();
}

TEST_CASE("runner: step() advances exactly the requested number of days while paused") {
    SimulationRunner runner(demo::make_simulation(small_config()));
    runner.start();
    runner.step(10);
    REQUIRE(runner.wait_for_ticks(10, 5000ms));
    std::this_thread::sleep_for(50ms);
    CHECK(runner.ticks_done() == 10);

    const auto snapshot = runner.latest_snapshot();
    CHECK(snapshot->today() == Date::from_ymd(1703, 1, 1) + 10);
    runner.stop();
}

TEST_CASE("runner: unlimited speed runs freely and pausing stops it with a final snapshot") {
    SimulationRunner runner(demo::make_simulation(small_config()));
    runner.start();
    runner.set_speed(Speed::Unlimited);
    REQUIRE(runner.wait_for_ticks(2000, 20000ms));

    runner.set_speed(Speed::Paused);
    std::this_thread::sleep_for(100ms);  // let the in-flight tick finish
    const std::int64_t stopped_at = runner.ticks_done();
    std::this_thread::sleep_for(100ms);
    CHECK(runner.ticks_done() == stopped_at);  // really paused

    // The published snapshot is the final state, not an older one.
    const auto snapshot = runner.latest_snapshot();
    CHECK(snapshot->calendar().tick_count() == stopped_at);
    runner.stop();
}

TEST_CASE("runner: paced speeds follow the configured rate and never skip days") {
    RunnerConfig config;
    config.days_per_second = {0.0, 100.0, 200.0, 400.0, 800.0, 1600.0, 0.0};
    SimulationRunner runner(demo::make_simulation(small_config()), config);
    runner.start();

    const auto begin = SimulationRunner::Clock::now();
    runner.set_speed(Speed::X1);  // 100 days per second
    REQUIRE(runner.wait_for_ticks(30, 10000ms));
    const auto elapsed = SimulationRunner::Clock::now() - begin;
    runner.set_speed(Speed::Paused);

    // 30 days at 100 days/s take about 0.3 s; allow slack for slow machines but not for "too fast".
    CHECK(elapsed >= 200ms);

    std::this_thread::sleep_for(50ms);
    const auto snapshot = runner.latest_snapshot();
    CHECK(snapshot->calendar().tick_count() == runner.ticks_done());
    CHECK(snapshot->today() == Date::from_ymd(1703, 1, 1) + static_cast<int>(runner.ticks_done()));
    runner.stop();
}

TEST_CASE("runner: changing speed takes effect promptly") {
    RunnerConfig config;
    config.days_per_second = {0.0, 2.0, 3.0, 8.0, 20.0, 40.0, 0.0};
    SimulationRunner runner(demo::make_simulation(small_config()), config);
    runner.start();
    runner.set_speed(Speed::X1);  // 2 days/s: far too slow for the check below
    std::this_thread::sleep_for(50ms);

    runner.set_speed(Speed::Unlimited);  // must wake the sleeping thread immediately
    CHECK(runner.wait_for_ticks(500, 10000ms));
    runner.stop();
}

TEST_CASE("runner: snapshots seen by another thread are consistent and monotonic") {
    SimulationRunner runner(demo::make_simulation(small_config(3)));
    runner.start();
    runner.set_speed(Speed::Unlimited);

    std::atomic<bool> stop{false};
    std::atomic<int> problems{0};
    std::atomic<int> observed{0};
    std::thread reader([&] {
        std::int64_t last_tick = -1;
        std::uint64_t last_version = 0;
        while (!stop.load()) {
            const std::uint64_t version = runner.snapshot_version();
            const auto snapshot = runner.latest_snapshot();
            const std::int64_t tick = snapshot->calendar().tick_count();
            if (tick < last_tick || version < last_version) {
                problems.fetch_add(1);
            }
            if (!snapshot->check_invariants().empty()) {
                problems.fetch_add(1);
            }
            // The date always matches the tick counter: no half-updated snapshot.
            if (snapshot->today() != Date::from_ymd(1703, 1, 1) + static_cast<int>(tick)) {
                problems.fetch_add(1);
            }
            last_tick = tick;
            last_version = version;
            observed.fetch_add(1);
        }
    });

    REQUIRE(runner.wait_for_ticks(3000, 30000ms));
    stop.store(true);
    reader.join();
    runner.stop();

    CHECK(problems.load() == 0);
    CHECK(observed.load() > 0);
}

TEST_CASE("runner: commands submitted from the UI thread are applied") {
    SimulationRunner runner(demo::make_simulation(small_config()));
    runner.start();

    const DynastyId target(0);
    const std::int32_t before = runner.latest_snapshot()->dynasties().honor[target.index()];
    for (int i = 0; i < 50; ++i) {
        REQUIRE(runner.submit(Command{DynastyId(), cmd::AdjustHonor{target, 1}}));
    }
    runner.step(2);
    REQUIRE(runner.wait_for_ticks(2, 5000ms));
    std::this_thread::sleep_for(30ms);

    const std::int32_t after = runner.latest_snapshot()->dynasties().honor[target.index()];
    CHECK(after == before + 50);
    runner.stop();
}

TEST_CASE("runner: a run through the thread equals a direct run (determinism is not affected by threading)") {
    const int days = 500;
    Simulation direct = demo::make_simulation(small_config(8));
    direct.run_days(days);

    SimulationRunner runner(demo::make_simulation(small_config(8)));
    runner.start();
    runner.set_speed(Speed::Unlimited);
    REQUIRE(runner.wait_for_ticks(days, 30000ms));
    runner.set_speed(Speed::Paused);
    runner.stop();

    // The runner may have run a few extra ticks before pausing; compare at exactly `days` by replaying.
    Simulation& sim = runner.simulation_when_stopped();
    REQUIRE(sim.tick_count() >= days);
    Simulation again = demo::make_simulation(small_config(8));
    again.run_days(static_cast<int>(sim.tick_count()));
    CHECK(sim.world().compute_hash() == again.world().compute_hash());
    CHECK(again.world().today() == sim.world().today());
    CHECK(direct.tick_count() == days);
}

TEST_CASE("runner: stop() is idempotent and the destructor stops a running thread") {
    {
        SimulationRunner runner(demo::make_simulation(small_config()));
        runner.start();
        runner.set_speed(Speed::Unlimited);
        std::this_thread::sleep_for(20ms);
        runner.stop();
        runner.stop();
        CHECK_FALSE(runner.running());
    }
    {
        SimulationRunner runner(demo::make_simulation(small_config()));
        runner.start();
        runner.set_speed(Speed::Unlimited);
        std::this_thread::sleep_for(20ms);
    }  // destructor joins without hanging
    CHECK(true);
}
