#pragma once

#include "pat/sim/simulation.hpp"
#include "pat/sim/world.hpp"

#include <cstdint>

// A small self-contained scenario used by tools (simcli), golden tests and benchmarks until real
// content loading exists (E4). It is NOT game content: it only exercises the simulation machinery
// - births, ageing, succession, province growth and honor rewards - through the real
// command/system/RNG paths.

namespace pat::sim::demo {

struct Config {
    std::uint64_t seed = 1;
    std::uint32_t dynasties = 8;
    std::uint32_t provinces = 80;
    std::uint32_t characters_per_dynasty = 6;
};

/// Identifies this world+systems combination in replay files ("demo.v1").
inline constexpr std::uint64_t kScenarioId = 0xB6D7A3E1C95F4021ULL;

/// Start date 1 January 1703 (Julian), deterministic for a given config.
[[nodiscard]] World make_world(const Config& config);

/// Registers the demo systems (births, ageing, growth, succession, rewards) in a fixed order.
void install_systems(Simulation& simulation);

[[nodiscard]] Simulation make_simulation(const Config& config, SimulationConfig simulation_config = {});

}  // namespace pat::sim::demo
