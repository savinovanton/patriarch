#pragma once

#include "pat/core/result.hpp"
#include "pat/mapdata/map_data.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace pat::mapdata {

/// Checks every invariant of a map (E3-T03 acceptance):
///  - layer sizes match width x height; materials are valid;
///  - province pixels are non-zero exactly on land, ids are within 1..N;
///  - each province record matches the pixels (area, bounding box, centroid inside the box);
///  - every province is one 4-connected region;
///  - the adjacency lists agree with the pixels, are symmetric and sorted;
///  - the province graph is connected;
///  - province colours are unique.
/// Returns the first violation found.
[[nodiscard]] core::Result<void, core::Error> validate_map(const MapData& map);

/// Number of provinces reachable from province 1 through the neighbour lists.
[[nodiscard]] std::size_t reachable_province_count(const MapData& map);

/// Per-pixel surface normals derived from the heights: two signed bytes (x, y components of the unit
/// normal scaled to [-127, 127]); z is reconstructed as sqrt(1 - x^2 - y^2). `metres_per_pixel` is
/// the horizontal size of a pixel. Output has width*height*2 entries.
[[nodiscard]] std::vector<std::int8_t> compute_normals(const MapData& map, double metres_per_pixel);

}  // namespace pat::mapdata
