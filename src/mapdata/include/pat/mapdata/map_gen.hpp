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

/// Province value left on land that no province reached (see partition_provinces).
inline constexpr std::uint16_t kUnassignedProvince = 0xFFFF;

/// The province split of generate_map on any land mask (MapData::is_land; heights only decide land
/// or water): seeded sampling of about `province_count` (at most 4000) land points, a warped Voronoi
/// split and a repair that keeps every province one 4-connected region. Fills `map.provinces`
/// (0 on water) and returns the number of provinces. Land that is not connected to any province
/// (an island without a seed) keeps kUnassignedProvince; the caller decides what to do with it.
/// Deterministic for a given (map, count, seed), whatever the number of threads.
[[nodiscard]] std::uint32_t partition_provinces(MapData& map,
                                                std::size_t land_pixels,
                                                std::uint32_t province_count,
                                                std::uint64_t seed,
                                                unsigned threads = 0);

/// Colour of province `id` in generated maps (golden-ratio hue steps).
[[nodiscard]] std::uint32_t generated_province_color(std::uint32_t id);

}  // namespace pat::mapdata
