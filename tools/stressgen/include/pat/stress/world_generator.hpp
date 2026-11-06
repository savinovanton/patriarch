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
};

// Constructs synthetic data directly, without simulating the intervening decades. This is
// benchmark input, not historical content or an implementation of the future game systems.
[[nodiscard]] core::Result<sim::World> generate_world(const WorldConfig& config);

}  // namespace pat::stress
