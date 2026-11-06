#pragma once

#include <cstdint>

namespace pat::sim {

/// Placeholder entry point of the simulation library; real world state arrives in E1.
/// Returns the number of simulated days for a given number of ticks.
[[nodiscard]] std::int64_t days_for_ticks(std::int64_t ticks) noexcept;

}  // namespace pat::sim
