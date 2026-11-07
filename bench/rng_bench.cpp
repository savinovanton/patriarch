#include "pat/core/rng.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>

namespace pat::bench {
namespace {

// Template for new workloads: deterministic setup outside the timed loop, a batch per
// iteration, observable output, and an item count for throughput. Never time file I/O here.
void pcg32_next(benchmark::State& state) {
    core::Pcg32 rng(42, 54);
    const auto draws = state.range(0);
    const auto work = draws + draws / 5;
    for (auto _ : state) {
        std::uint64_t checksum = 0;
        for (std::int64_t i = 0; i < work; ++i) {
            checksum += rng.next_u32();
        }
        benchmark::DoNotOptimize(checksum);
    }
    state.SetItemsProcessed(state.iterations() * draws);
}

BENCHMARK(pcg32_next)->Name("core.pcg32_next")->Arg(1024)->Arg(16384);

}  // namespace
}  // namespace pat::bench
