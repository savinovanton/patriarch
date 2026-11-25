#pragma once

#include "pat/core/result.hpp"
#include "pat/stress/dataset.hpp"

#include <cstdint>
#include <string>

// The benchmark of the event dispatcher (E11-T05, docs/benchmarks/E11-T05-dispatch.md): the dispatcher of
// pat::sim::events runs by stage on a generated world with synthetic sets of events (the M1 mix of 110,
// the E2-T04 mix of 500 and the release mix of 550, docs/dev/event-engine.md section 12.6) and two streams
// of commands (a realistic one and the dense one of E2-T04). The commands are handed to the dispatcher as
// the log of the tick without being applied: the cost of the producers and the gates is measured, the
// world changes only through the launches (history, cooldowns). Every scenario is run a second time with
// the full scan, and the candidates and launches of both are compared on every day.

namespace pat::stress {

struct EventsBenchOptions {
    std::uint32_t days = 365;       // measured days
    std::uint32_t warmup = 31;      // days run before the measurement
    std::uint64_t seed = 20261122;  // the synthetic events and commands
};

[[nodiscard]] core::Result<std::string> run_events_bench(const Config& config,
                                                         const EventsBenchOptions& options);

}  // namespace pat::stress
