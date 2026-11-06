#include <doctest/doctest.h>

#include <atomic>
#include <vector>

#include "pool_executor.hpp"
#include "workload.hpp"

using namespace pat;

TEST_CASE("tick spike: pool covers uneven batches and can be reused") {
    spike::PoolExecutor pool(4);
    std::vector<std::atomic<int>> visits(1003);
    for (const auto grain : {0U, 1U, 64U, 256U, 2000U}) {
        for (auto& visit : visits) {
            visit.store(0);
        }
        pool.parallel_for(visits.size(), grain, [&](std::size_t first, std::size_t last) {
            for (auto i = first; i < last; ++i) {
                visits[i].fetch_add(1);
            }
        });
        for (const auto& visit : visits) {
            CHECK(visit.load() == 1);
        }
    }
    int called = 0;
    pool.parallel_for(0, 0, [&](std::size_t, std::size_t) { ++called; });
    CHECK(called == 0);
}

TEST_CASE("tick spike: threads and monthly scheduling preserve a full month's state") {
    auto config = stress::profile_config("year60").value();
    config.world.provinces = 57;
    config.world.characters = 120;
    config.world.dynasties = 4;
    config.armies = 17;
    config.schemes = 9;
    config.contracts = 8;
    config.events = 10;
    auto generated = spike::make_state(config);
    REQUIRE(generated.has_value());
    sim::SerialExecutor serial;
    spike::PoolExecutor pool(4);
    std::uint64_t expected = 0;
    for (const auto mode : {spike::MonthlyMode::Burst, spike::MonthlyMode::Stagger}) {
        for (const auto grain : {1U, 64U}) {
            auto a = generated.value();
            auto b = generated.value();
            for (int day = 0; day < 31; ++day) {
                const auto sa = spike::tick(a, serial, grain, mode, 64);
                const auto sb = spike::tick(b, pool, grain, mode, 64);
                CHECK(sa.ai_wu == 64);
                CHECK(sb.ai_wu == sa.ai_wu);
                if (mode == spike::MonthlyMode::Stagger && day >= 28) {
                    CHECK(sa.stage_us[spike::Economy] == 0);
                    CHECK(sa.stage_us[spike::Trade] == 0);
                }
            }
            CHECK(a.work[spike::Economy] == 57);
            CHECK(a.work[spike::Trade] == a.nodes * 10);
            CHECK(a.work[spike::Demography] == 120);
            CHECK(a.work[spike::Ai] == 31 * 64);
            CHECK(spike::state_hash(a) == spike::state_hash(b));
            if (expected == 0) {
                expected = spike::state_hash(a);
            }
            CHECK(spike::state_hash(a) == expected);
        }
    }
    auto state = generated.value();
    CHECK(spike::tick(state, pool, 64, spike::MonthlyMode::Stagger, 31).ai_wu == 0);
}
