#pragma once

namespace pat::bench {

// Shared runner: warmup, repetitions, JSON output and optional CPU pinning. Native Google
// Benchmark flags override the defaults. CPU affinity affects this process's main thread only.
int run_benchmarks(int argc, char** argv);

}  // namespace pat::bench
