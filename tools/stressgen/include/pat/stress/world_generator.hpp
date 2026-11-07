#pragma once

#include "pat/core/result.hpp"
#include "pat/sim/world.hpp"

#include <cstdint>

namespace pat::stress {

struct WorldConfig {
    std::uint64_t seed = 1;
    std::uint32_t provinces = 400;
    std::uint32_t characters = 5000;
    std::uint32_t dynasties = 100;
    sim::Date date = sim::Date::from_ymd(1762, 1, 1);
    /// Build the world in dynasty mode (ADR-0006): ids are never reused and the life-event queue is
    /// populated. This is the shape of the real game; false gives the E1 shape of the old demo.
    bool dynasty_mode = true;
};

// Constructs synthetic data directly, without simulating the intervening decades. This is
// benchmark input, not historical content or an implementation of the future game systems.
[[nodiscard]] core::Result<sim::World> generate_world(const WorldConfig& config);

}  // namespace pat::stress
