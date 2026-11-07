#include "pat/sim/demo_scenario.hpp"

#include <benchmark/benchmark.h>

#include <chrono>
#include <string>

namespace pat::bench {
namespace {

// Each measured batch starts from the same world. Restore, summary sorting/hash and I/O are
// outside timing. The outer wall interval bounds the observer's inclusive sim.tick duration.
void telemetry_tick(benchmark::State& state) {
    auto simulation = sim::demo::make_simulation({7, 100, 400, 50});
    const auto base = simulation.world();
    const bool enabled = state.range(0) != 0;
    constexpr int kTicks = 32;
    if (enabled && !simulation.enable_telemetry(kTicks)) {
        state.SkipWithError("cannot enable telemetry");
        return;
    }
    double wall_ns = 0, observer_ns = 0;
    std::uint64_t ticks = 0;
    for (auto _ : state) {
        state.PauseTiming();
        simulation.restore_state(base, {});
        state.ResumeTiming();
        const auto begin = std::chrono::steady_clock::now();
        simulation.run_days(kTicks);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin)
                .count();
        state.PauseTiming();
        wall_ns += static_cast<double>(elapsed);
        ticks += kTicks;
        if (enabled) {
            const auto snapshot = simulation.telemetry_snapshot();
            const auto& metric = snapshot->metrics.front();
            if (snapshot->frames != kTicks || metric.calls != kTicks ||
                metric.per_tick.total_ns > static_cast<std::uint64_t>(elapsed)) {
                state.SkipWithError("telemetry frame count or inclusive interval differs from outer timer");
                state.ResumeTiming();
                break;
            }
            observer_ns += static_cast<double>(metric.per_tick.total_ns);
        }
        state.ResumeTiming();
    }
    const auto denominator = static_cast<double>(ticks);
    state.counters["wall_ns_per_tick"] = denominator ? wall_ns / denominator : 0;
    state.counters["observer_ns_per_tick"] = denominator ? observer_ns / denominator : 0;
    state.counters["observed_ticks"] = static_cast<double>(enabled ? ticks : 0);
    state.SetLabel(std::to_string(simulation.world().compute_hash().total));
    state.SetItemsProcessed(static_cast<std::int64_t>(ticks));
}

BENCHMARK(telemetry_tick)->Name("sim.telemetry_tick")->Arg(0)->Arg(1)->UseRealTime();

}  // namespace
}  // namespace pat::bench
