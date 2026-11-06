#pragma once

#include "pat/mapdata/map_data.hpp"

#include <cstdint>

// Procedural test map generator (E3-T03): a continent with a mountain ridge, a coastline, ~400 land
// provinces and material classes. Deterministic for a given (params, platform): the same seed always
// produces the same map regardless of the number of threads. This is offline tooling - it uses
// floating point and threads freely and must never be linked into the simulation.

namespace pat::mapdata {

struct GenParams {
    std::uint32_t width = 8192;
    std::uint32_t height = 4096;
    std::uint64_t seed = 1;
    std::uint32_t province_count = 400;
    /// Worker threads; 0 picks the hardware concurrency.
    unsigned threads = 0;
};

/// Height value of the waterline in generated maps.
inline constexpr std::uint16_t kGeneratedSeaLevel = 16384;

[[nodiscard]] MapData generate_map(const GenParams& params);

}  // namespace pat::mapdata
