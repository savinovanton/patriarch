#pragma once

#include "pat/mapdata/map_data.hpp"

#include <cstddef>
#include <vector>

namespace pat::mapdata {

/// Derives the per-province records (area, bounding box, centroid, mean height, coast flag and
/// neighbour lists with border lengths) from the province layer. Colours are left zero.
[[nodiscard]] std::vector<Province> compute_province_stats(const MapData& map, std::size_t province_count);

/// Recomputes everything in `map.province_list` except the ids and colours from the pixel layers.
void rebuild_province_stats(MapData& map);

}  // namespace pat::mapdata
